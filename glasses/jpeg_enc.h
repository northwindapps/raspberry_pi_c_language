#ifndef JPEG_ENC_H
#define JPEG_ENC_H

#include <stdint.h>

/* Both return 0 on success. *out is malloc'd by libjpeg; free() it. */
int jpeg_encode_gray(const uint8_t *gray, int w, int h, int stride, int quality,
                     unsigned char **out, unsigned long *out_len);

int jpeg_encode_yuv420(const uint8_t *y, const uint8_t *u, const uint8_t *v,
                       int w, int h, int y_stride, int uv_stride, int quality,
                       unsigned char **out, unsigned long *out_len);

#endif
