/*
 * camera.h - plain C interface to the camera.
 *
 * libcamera only offers a C++ API, so camera.cpp wraps it and everything
 * else in this program stays in C.
 */
#ifndef CAMERA_H
#define CAMERA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One finished frame. Pointers are only valid inside the callback. */
typedef struct {
    /* Main stream, YUV420: the Y plane alone is the grayscale image */
    const uint8_t *main_y;
    int main_w, main_h, main_stride;

    /* Low-res stream, YUV420 (planar), for the live preview */
    const uint8_t *lores_y, *lores_u, *lores_v;
    int lores_w, lores_h, lores_stride, lores_uv_stride;
} cam_frame_t;

/* Called on the camera thread for every frame. Keep it short. */
typedef void (*cam_frame_cb)(const cam_frame_t *frame, void *user);

/* Open the first camera, configure both streams and start streaming. */
int cam_start(int main_w, int main_h, int lores_w, int lores_h,
              cam_frame_cb cb, void *user);
void cam_stop(void);

#ifdef __cplusplus
}
#endif

#endif
