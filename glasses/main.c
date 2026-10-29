/*
 * main.c - smart glasses camera/audio API, a drop-in replacement for
 * app_shoot.py with the same endpoints on port 5000:
 *
 *   GET/POST /api/capture       grayscale JPEG, rotated 90° left
 *   GET  /api/video/stream      MJPEG preview (?format=raw for octet-stream)
 *   GET  /api/audio?seconds=N   last N seconds as WAV
 *   GET  /api/audio/stream      endless 16 kHz mono int16 PCM (?format=wav)
 *   POST /api/shutdown          halt the Pi
 *
 * Big buffers are allocated once at startup and reused.
 */
#include "audio.h"
#include "camera.h"
#include "http.h"
#include "jpeg_enc.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* IMX708 2x2 binned mode: full field of view (full-res is too heavy for the Zero 2 W) */
#define MAIN_W 2304
#define MAIN_H 1296
/* Live preview, same 16:9 as the main stream */
#define LORES_W 640
#define LORES_H 360
#define VIDEO_MAX_FPS   15
#define VIDEO_QUALITY   80
#define CAPTURE_QUALITY 95
#define CAPTURE_TIMEOUT 5     /* seconds to wait for a frame before giving up */

static const char JSON_CAM_DEAD[] =
    "{\"status\":\"error\",\"message\":\"Camera not responding, restarting\"}";

/* --- Photo capture: the HTTP thread asks, the camera thread fills cap_buf --- */
static pthread_mutex_t capture_lock = PTHREAD_MUTEX_INITIALIZER;  /* one capture at a time */
static pthread_mutex_t cap_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cap_cond = PTHREAD_COND_INITIALIZER;
static int cap_requested, cap_ready;
static uint8_t *cap_buf;           /* rotated grayscale, MAIN_H wide x MAIN_W tall */
static int cap_w, cap_h;

/* --- Live preview: the camera thread encodes, stream threads send --- */
static pthread_mutex_t video_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t video_cond = PTHREAD_COND_INITIALIZER;
static int video_viewers;
static unsigned char *video_jpeg;
static unsigned long video_len;
static unsigned long video_seq;
static double video_last;

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void deadline_in(struct timespec *ts, int seconds)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += seconds;
}

/* Rotate 90° counter-clockwise (corrects the camera's mounting), in 32x32
 * tiles so both source and destination stay in cache. */
static void rotate_ccw(const uint8_t *src, int w, int h, int stride, uint8_t *dst)
{
    enum { T = 32 };
    for (int by = 0; by < h; by += T) {
        for (int bx = 0; bx < w; bx += T) {
            int ey = by + T < h ? by + T : h;
            int ex = bx + T < w ? bx + T : w;
            for (int y = by; y < ey; y++) {
                const uint8_t *s = src + (size_t)y * stride;
                for (int x = bx; x < ex; x++)
                    dst[(size_t)(w - 1 - x) * h + y] = s[x];
            }
        }
    }
}

/* Runs on the camera thread for every frame */
static void on_frame(const cam_frame_t *f, void *user)
{
    (void)user;

    pthread_mutex_lock(&cap_mtx);
    if (cap_requested) {
        rotate_ccw(f->main_y, f->main_w, f->main_h, f->main_stride, cap_buf);
        cap_w = f->main_h;
        cap_h = f->main_w;
        cap_requested = 0;
        cap_ready = 1;
        pthread_cond_signal(&cap_cond);
    }
    pthread_mutex_unlock(&cap_mtx);

    /* Only encode preview frames while someone is watching (saves CPU and heat) */
    pthread_mutex_lock(&video_mtx);
    int want = video_viewers > 0 && now_sec() - video_last >= 1.0 / VIDEO_MAX_FPS;
    pthread_mutex_unlock(&video_mtx);
    if (!want)
        return;

    unsigned char *jpg;
    unsigned long len;
    if (jpeg_encode_yuv420(f->lores_y, f->lores_u, f->lores_v, f->lores_w, f->lores_h,
                           f->lores_stride, f->lores_uv_stride, VIDEO_QUALITY, &jpg, &len))
        return;

    pthread_mutex_lock(&video_mtx);
    free(video_jpeg);
    video_jpeg = jpg;
    video_len = len;
    video_seq++;
    video_last = now_sec();
    pthread_cond_broadcast(&video_cond);
    pthread_mutex_unlock(&video_mtx);
}

static void *exit_soon(void *arg)
{
    (void)arg;
    usleep(500 * 1000);   /* let the 503 reach the client first */
    _exit(1);
}

/* Called by iOS when the wake word is detected -> capture a photo and return JPEG */
static void handle_capture(http_req_t *req)
{
    struct timespec deadline;
    int timed_out = 0;

    pthread_mutex_lock(&capture_lock);

    pthread_mutex_lock(&cap_mtx);
    cap_ready = 0;
    cap_requested = 1;
    deadline_in(&deadline, CAPTURE_TIMEOUT);
    while (!cap_ready && !timed_out)
        timed_out = pthread_cond_timedwait(&cap_cond, &cap_mtx, &deadline) == ETIMEDOUT;
    cap_requested = 0;
    pthread_mutex_unlock(&cap_mtx);

    if (timed_out) {
        /* Camera stopped sending frames (loose cable etc.): exit so systemd
         * (Restart=always) restarts the service and re-opens the camera */
        pthread_t th;
        printf("[camera] no frame for %ds -> exiting so systemd restarts the camera\n",
               CAPTURE_TIMEOUT);
        pthread_mutex_unlock(&capture_lock);
        pthread_create(&th, NULL, exit_soon, NULL);
        http_respond_json(req, 503, JSON_CAM_DEAD);
        return;
    }

    unsigned char *jpg;
    unsigned long len;
    int err = jpeg_encode_gray(cap_buf, cap_w, cap_h, cap_w, CAPTURE_QUALITY, &jpg, &len);
    pthread_mutex_unlock(&capture_lock);

    if (err) {
        http_respond_json(req, 500, "{\"status\":\"error\",\"message\":\"Encode failed\"}");
        return;
    }
    http_respond(req, 200, "image/jpeg", jpg, len, NULL);
    free(jpg);
}

/* Live preview as MJPEG: one endless response, each part is one JPEG frame.
 * ?format=raw sends the same bytes as octet-stream (iOS URLSession splits
 * multipart/x-mixed-replace into separate responses). */
static void handle_video(http_req_t *req)
{
    char fmt[16];
    int want_raw = http_query(req, "format", fmt, sizeof(fmt)) && strcmp(fmt, "raw") == 0;
    unsigned char *frame = NULL;
    size_t cap = 0;

    pthread_mutex_lock(&video_mtx);
    if (video_viewers++ == 0)
        printf("[video] encoder started\n");
    unsigned long seen = video_seq;
    pthread_mutex_unlock(&video_mtx);

    if (http_start_chunked(req, want_raw ? "application/octet-stream"
                                         : "multipart/x-mixed-replace; boundary=frame") == 0) {
        for (;;) {
            struct timespec deadline;
            int timed_out = 0;
            unsigned long len;

            pthread_mutex_lock(&video_mtx);
            deadline_in(&deadline, 5);
            while (video_seq == seen && !timed_out)
                timed_out = pthread_cond_timedwait(&video_cond, &video_mtx, &deadline) == ETIMEDOUT;
            if (timed_out) {
                /* No frames (camera stalled) -> end response, client reconnects */
                pthread_mutex_unlock(&video_mtx);
                http_end_chunked(req);
                break;
            }
            seen = video_seq;
            len = video_len;
            if (len > cap) {
                free(frame);
                cap = len * 2;
                frame = malloc(cap);
            }
            if (frame)
                memcpy(frame, video_jpeg, len);
            pthread_mutex_unlock(&video_mtx);
            if (!frame)
                break;

            char head[96];
            struct iovec parts[3] = {
                { head, snprintf(head, sizeof(head),
                                 "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %lu\r\n\r\n",
                                 len) },
                { frame, len },
                { "\r\n", 2 },
            };
            if (http_send_chunk(req, parts, 3))
                break;   /* client went away */
        }
    }
    free(frame);

    pthread_mutex_lock(&video_mtx);
    if (--video_viewers == 0)
        printf("[video] encoder stopped\n");
    pthread_mutex_unlock(&video_mtx);
}

static void put_le(uint8_t *p, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; i++)
        p[i] = v >> (8 * i);
}

/* 44-byte header for 16 kHz mono int16 PCM */
static void wav_header(uint8_t h[44], uint32_t data_size)
{
    memcpy(h, "RIFF", 4);
    put_le(h + 4, data_size + 36, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le(h + 16, 16, 4);
    put_le(h + 20, 1, 2);                 /* PCM */
    put_le(h + 22, 1, 2);                 /* mono */
    put_le(h + 24, AUDIO_RATE, 4);
    put_le(h + 28, AUDIO_RATE * 2, 4);    /* byte rate */
    put_le(h + 32, 2, 2);                 /* block align */
    put_le(h + 34, 16, 2);                /* bits */
    memcpy(h + 36, "data", 4);
    put_le(h + 40, data_size, 4);
}

/* Return the last N seconds as WAV (for polling)  e.g. /api/audio?seconds=2 */
static void handle_audio(http_req_t *req)
{
    char val[32];
    double seconds = 2;
    if (http_query(req, "seconds", val, sizeof(val)))
        seconds = atof(val);
    if (seconds > AUDIO_BUFFER_SECONDS)
        seconds = AUDIO_BUFFER_SECONDS;
    if (seconds < 0)
        seconds = 0;

    size_t want = (size_t)(seconds * AUDIO_RATE);
    uint8_t *wav = malloc(44 + want * sizeof(int16_t));
    if (!wav) {
        http_respond_json(req, 500, "{\"status\":\"error\",\"message\":\"Out of memory\"}");
        return;
    }
    size_t got = audio_get_recent((int16_t *)(wav + 44), want);
    wav_header(wav, got * sizeof(int16_t));
    http_respond(req, 200, "audio/wav", wav, 44 + got * sizeof(int16_t),
                 "Cache-Control: no-cache\r\n");
    free(wav);
}

/* Continuously stream raw 16 kHz mono int16 PCM (?format=wav adds a WAV header) */
static void handle_audio_stream(http_req_t *req)
{
    char fmt[16];
    int want_wav = http_query(req, "format", fmt, sizeof(fmt)) && strcmp(fmt, "wav") == 0;
    audio_sub_t *sub = audio_subscribe();
    int16_t chunk[AUDIO_CHUNK];

    if (!sub) {
        http_respond_json(req, 503, "{\"status\":\"error\",\"message\":\"Too many audio clients\"}");
        return;
    }
    if (http_start_chunked(req, "application/octet-stream") == 0) {
        int ok = 1;
        if (want_wav) {
            uint8_t h[44];
            wav_header(h, 0xFFFFFFFFu - 36);  /* size unknown when streaming: use the max */
            struct iovec part = { h, sizeof(h) };
            ok = http_send_chunk(req, &part, 1) == 0;
        }
        while (ok) {
            if (!audio_sub_read(sub, chunk))
                continue;
            struct iovec part = { chunk, sizeof(chunk) };
            ok = http_send_chunk(req, &part, 1) == 0;
        }
    }
    audio_unsubscribe(sub);
}

static void *halt_soon(void *arg)
{
    (void)arg;
    sleep(1);   /* let the response go out before halting */
    if (system("sudo halt") != 0)
        printf("[shutdown] sudo halt failed\n");
    return NULL;
}

static void handle_shutdown(http_req_t *req)
{
    pthread_t th;
    if (pthread_create(&th, NULL, halt_soon, NULL) == 0)
        pthread_detach(th);
    http_respond_json(req, 200, "{\"status\":\"ok\",\"message\":\"Shutting down\"}");
}

static void route(http_req_t *req)
{
    static const struct {
        const char *path;
        void (*fn)(http_req_t *);
        int allow_get, allow_post;
    } routes[] = {
        { "/api/capture",      handle_capture,      1, 1 },
        { "/api/video/stream", handle_video,        1, 0 },
        { "/api/audio",        handle_audio,        1, 0 },
        { "/api/audio/stream", handle_audio_stream, 1, 0 },
        { "/api/shutdown",     handle_shutdown,     0, 1 },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        if (strcmp(req->path, routes[i].path) != 0)
            continue;
        if ((strcmp(req->method, "GET") == 0 && routes[i].allow_get) ||
            (strcmp(req->method, "POST") == 0 && routes[i].allow_post))
            routes[i].fn(req);
        else
            http_respond_json(req, 405, "{\"status\":\"error\",\"message\":\"Method not allowed\"}");
        return;
    }
    http_respond_json(req, 404, "{\"status\":\"error\",\"message\":\"Not found\"}");
}

static void *server_thread(void *arg)
{
    if (http_serve(*(int *)arg, route) != 0)
        _exit(1);
    return NULL;
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : 5000;
    sigset_t sigs;
    pthread_t th;
    int sig;

    setvbuf(stdout, NULL, _IOLBF, 0);   /* log lines show up in journalctl right away */
    signal(SIGPIPE, SIG_IGN);

    /* Handle Ctrl+C / systemctl stop in main only, so the camera is released cleanly */
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL);

    cap_buf = malloc((size_t)MAIN_W * MAIN_H);
    if (!cap_buf)
        return 1;

    /* 0. Start the camera */
    if (cam_start(MAIN_W, MAIN_H, LORES_W, LORES_H, on_frame, NULL) != 0)
        return 1;
    printf("InnoMaker IMX708 Camera Active (AF-Continuous)...\n");

    /* 1. Start audio capture on its own thread
     * (from here on, fatal errors use _exit: the camera thread is running and
     * C++ aborts if exit() destroys it) */
    if (audio_start() != 0)
        _exit(1);

    /* 2. Start the HTTP server (0.0.0.0 so the iOS app can reach it) */
    if (pthread_create(&th, NULL, server_thread, &port) != 0)
        _exit(1);

    sigwait(&sigs, &sig);
    printf("Stopping (signal %d)...\n", sig);
    cam_stop();
    return 0;
}
