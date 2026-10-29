/*
 * jpeg_enc.c - JPEG encoding straight from the camera's planes.
 *
 * The camera already delivers YCbCr, which is what JPEG stores internally,
 * so we feed the planes to libjpeg as-is ("raw data") instead of converting
 * to RGB and back.
 */
#include "jpeg_enc.h"

#include <setjmp.h>
#include <stdio.h>
#include <jpeglib.h>

/* libjpeg's default error handler calls exit(); jump back instead. */
typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
} err_mgr_t;

static void on_error(j_common_ptr cinfo)
{
    err_mgr_t *err = (err_mgr_t *)cinfo->err;
    (*cinfo->err->output_message)(cinfo);
    longjmp(err->jump, 1);
}

int jpeg_encode_gray(const uint8_t *gray, int w, int h, int stride, int quality,
                     unsigned char **out, unsigned long *out_len)
{
    struct jpeg_compress_struct cinfo;
    err_mgr_t err;

    *out = NULL;
    *out_len = 0;
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = on_error;
    if (setjmp(err.jump)) {
        jpeg_destroy_compress(&cinfo);
        return -1;
    }

    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, out, out_len);
    cinfo.image_width = w;
    cinfo.image_height = h;
    cinfo.input_components = 1;
    cinfo.in_color_space = JCS_GRAYSCALE;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = (JSAMPROW)(gray + (size_t)cinfo.next_scanline * stride);
        jpeg_write_scanlines(&cinfo, &row, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    return 0;
}

int jpeg_encode_yuv420(const uint8_t *y, const uint8_t *u, const uint8_t *v,
                       int w, int h, int y_stride, int uv_stride, int quality,
                       unsigned char **out, unsigned long *out_len)
{
    struct jpeg_compress_struct cinfo;
    err_mgr_t err;
    JSAMPROW y_rows[16], u_rows[8], v_rows[8];
    JSAMPARRAY planes[3] = { y_rows, u_rows, v_rows };

    *out = NULL;
    *out_len = 0;
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = on_error;
    if (setjmp(err.jump)) {
        jpeg_destroy_compress(&cinfo);
        return -1;
    }

    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, out, out_len);
    cinfo.image_width = w;
    cinfo.image_height = h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_YCbCr;
    jpeg_set_defaults(&cinfo);
    jpeg_set_colorspace(&cinfo, JCS_YCbCr);
    jpeg_set_quality(&cinfo, quality, TRUE);
    cinfo.raw_data_in = TRUE;
    cinfo.comp_info[0].h_samp_factor = 2;   /* 4:2:0 */
    cinfo.comp_info[0].v_samp_factor = 2;
    cinfo.comp_info[1].h_samp_factor = cinfo.comp_info[1].v_samp_factor = 1;
    cinfo.comp_info[2].h_samp_factor = cinfo.comp_info[2].v_samp_factor = 1;
    jpeg_start_compress(&cinfo, TRUE);

    /* Raw input goes in 16-row bands; rows past the bottom repeat the last row */
    while (cinfo.next_scanline < cinfo.image_height) {
        int top = cinfo.next_scanline;
        for (int i = 0; i < 16; i++) {
            int r = top + i < h ? top + i : h - 1;
            y_rows[i] = (JSAMPROW)(y + (size_t)r * y_stride);
        }
        for (int i = 0; i < 8; i++) {
            int r = top / 2 + i < (h + 1) / 2 ? top / 2 + i : (h + 1) / 2 - 1;
            u_rows[i] = (JSAMPROW)(u + (size_t)r * uv_stride);
            v_rows[i] = (JSAMPROW)(v + (size_t)r * uv_stride);
        }
        jpeg_write_raw_data(&cinfo, planes, 16);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    return 0;
}
