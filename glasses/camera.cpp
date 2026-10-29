/*
 * camera.cpp - libcamera wrapper behind the C API in camera.h.
 *
 * Frames arrive in DMA buffers shared with the ISP. We mmap each buffer
 * once at startup and hand the C side raw pointers into them, so no frame
 * data is copied here.
 */
#include "camera.h"

#include <libcamera/libcamera.h>
#include <sys/mman.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using namespace libcamera;

namespace {

struct Mapped {
    uint8_t *plane[3];
};

std::unique_ptr<CameraManager> g_mgr;
std::shared_ptr<Camera> g_cam;
std::unique_ptr<CameraConfiguration> g_config;
std::unique_ptr<FrameBufferAllocator> g_alloc;
std::vector<std::unique_ptr<Request>> g_requests;
std::map<const FrameBuffer *, Mapped> g_maps;
std::vector<std::pair<void *, size_t>> g_mmaps;
Stream *g_main, *g_lores;
cam_frame_t g_geom;              /* sizes/strides, filled once after configure */
cam_frame_cb g_cb;
void *g_user;

/* libcamera completes requests on its own thread; a worker runs the callback
 * so slow work never stalls libcamera's event loop. */
std::mutex g_mtx;
std::condition_variable g_cv;
std::deque<Request *> g_done;
bool g_running;
std::thread g_worker;

void on_request_complete(Request *req)
{
    if (req->status() == Request::RequestCancelled)
        return;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_done.push_back(req);
    }
    g_cv.notify_one();
}

void worker()
{
    for (;;) {
        Request *req;
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv.wait(lk, [] { return !g_running || !g_done.empty(); });
            if (!g_running)
                return;
            req = g_done.front();
            g_done.pop_front();
        }

        const Mapped &m = g_maps[req->findBuffer(g_main)];
        const Mapped &l = g_maps[req->findBuffer(g_lores)];
        cam_frame_t f = g_geom;
        f.main_y = m.plane[0];
        f.lores_y = l.plane[0];
        f.lores_u = l.plane[1];
        f.lores_v = l.plane[2];
        g_cb(&f, g_user);

        req->reuse(Request::ReuseBuffers);
        g_cam->queueRequest(req);
    }
}

int map_buffers(const StreamConfiguration &sc)
{
    for (const auto &buf : g_alloc->buffers(sc.stream())) {
        Mapped m{};
        std::map<int, uint8_t *> by_fd;    /* planes usually share one dmabuf */
        const auto &planes = buf->planes();

        for (size_t i = 0; i < planes.size() && i < 3; i++) {
            int fd = planes[i].fd.get();
            if (!by_fd.count(fd)) {
                size_t len = lseek(fd, 0, SEEK_END);
                void *p = mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, 0);
                if (p == MAP_FAILED) {
                    perror("[camera] mmap");
                    return -1;
                }
                g_mmaps.emplace_back(p, len);
                by_fd[fd] = static_cast<uint8_t *>(p);
            }
            m.plane[i] = by_fd[fd] + planes[i].offset;
        }
        if (planes.size() == 1) {          /* single-plane layout: derive U/V */
            m.plane[1] = m.plane[0] + sc.stride * sc.size.height;
            m.plane[2] = m.plane[1] + sc.stride / 2 * sc.size.height / 2;
        }
        g_maps[buf.get()] = m;
    }
    return 0;
}

} /* namespace */

extern "C" int cam_start(int main_w, int main_h, int lores_w, int lores_h,
                         cam_frame_cb cb, void *user)
{
    g_cb = cb;
    g_user = user;

    g_mgr = std::make_unique<CameraManager>();
    if (g_mgr->start() || g_mgr->cameras().empty()) {
        fprintf(stderr, "[camera] no camera found\n");
        return -1;
    }
    g_cam = g_mgr->cameras()[0];
    if (g_cam->acquire()) {
        fprintf(stderr, "[camera] camera busy (is another program using it?)\n");
        return -1;
    }

    g_config = g_cam->generateConfiguration({ StreamRole::Viewfinder, StreamRole::Viewfinder });
    StreamConfiguration &mc = g_config->at(0);
    StreamConfiguration &lc = g_config->at(1);
    mc.pixelFormat = lc.pixelFormat = formats::YUV420;
    mc.colorSpace = lc.colorSpace = ColorSpace::Sycc;   /* full-range, what JPEG expects */
    mc.size = Size(main_w, main_h);
    lc.size = Size(lores_w, lores_h);
    mc.bufferCount = lc.bufferCount = 3;               /* fewest that keeps frames flowing */

    if (g_config->validate() == CameraConfiguration::Invalid) {
        fprintf(stderr, "[camera] configuration rejected\n");
        return -1;
    }
    if (g_cam->configure(g_config.get())) {
        fprintf(stderr, "[camera] configure failed\n");
        return -1;
    }
    g_main = mc.stream();
    g_lores = lc.stream();
    printf("[camera] main %s stride %u, lores %s stride %u\n",
           mc.toString().c_str(), mc.stride, lc.toString().c_str(), lc.stride);

    g_geom = {};
    g_geom.main_w = mc.size.width;
    g_geom.main_h = mc.size.height;
    g_geom.main_stride = mc.stride;
    g_geom.lores_w = lc.size.width;
    g_geom.lores_h = lc.size.height;
    g_geom.lores_stride = lc.stride;
    g_geom.lores_uv_stride = lc.stride / 2;

    g_alloc = std::make_unique<FrameBufferAllocator>(g_cam);
    if (g_alloc->allocate(g_main) < 0 || g_alloc->allocate(g_lores) < 0 ||
        map_buffers(mc) || map_buffers(lc))
        return -1;

    const auto &mbufs = g_alloc->buffers(g_main);
    const auto &lbufs = g_alloc->buffers(g_lores);
    for (size_t i = 0; i < mbufs.size() && i < lbufs.size(); i++) {
        auto req = g_cam->createRequest();
        if (!req || req->addBuffer(g_main, mbufs[i].get()) ||
            req->addBuffer(g_lores, lbufs[i].get())) {
            fprintf(stderr, "[camera] cannot create request\n");
            return -1;
        }
        g_requests.push_back(std::move(req));
    }

    /* Same settings as the old picamera2 setup */
    ControlList ctrls(controls::controls);
    const ControlInfoMap &info = g_cam->controls();
    if (info.count(&controls::AfMode))
        ctrls.set(controls::AfMode, controls::AfModeContinuous);
    ctrls.set(controls::AwbMode, controls::AwbIncandescent);
    ctrls.set(controls::Saturation, 1.0f);
    const int64_t frame_limits[2] = { 33333, 83333 };  /* 12-30 fps */
    ctrls.set(controls::FrameDurationLimits, std::span<const int64_t, 2>(frame_limits));
    if (info.count(&controls::draft::NoiseReductionMode))
        ctrls.set(controls::draft::NoiseReductionMode, controls::draft::NoiseReductionModeMinimal);

    g_cam->requestCompleted.connect(on_request_complete);
    g_running = true;
    g_worker = std::thread(worker);

    if (g_cam->start(&ctrls)) {
        fprintf(stderr, "[camera] start failed\n");
        return -1;
    }
    for (auto &req : g_requests)
        g_cam->queueRequest(req.get());
    return 0;
}

extern "C" void cam_stop(void)
{
    if (!g_cam)
        return;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_running = false;
    }
    g_cv.notify_one();
    if (g_worker.joinable())
        g_worker.join();

    g_cam->stop();
    g_cam->requestCompleted.disconnect(on_request_complete);
    g_requests.clear();
    for (auto &[p, len] : g_mmaps)
        munmap(p, len);
    g_mmaps.clear();
    g_maps.clear();
    g_alloc.reset();
    g_cam->release();
    g_cam.reset();
    g_mgr->stop();
}
