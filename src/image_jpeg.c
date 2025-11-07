#include "image_jpeg.h"
#include <stdio.h>
#include <jpeglib.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Decode JPEG from memory to RGB (channels = 3) */
int jpeg_decode_image(const uint8_t* in_buf, size_t in_len,
                      uint8_t** out_pixels, size_t* out_len,
                      int* width, int* height, int* channels) {
    if (!in_buf || in_len < 4) return -1;
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPARRAY buffer; int row_stride;
    int ret = -1;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (unsigned char*)in_buf, (unsigned long)in_len);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) goto cleanup;
    /* Request RGB output */
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    row_stride = cinfo.output_width * cinfo.output_components;
    size_t pixels_size = (size_t)row_stride * cinfo.output_height;
    uint8_t* pixels = (uint8_t*)malloc(pixels_size ? pixels_size : 1);
    if (!pixels) goto cleanup;

    buffer = (*cinfo.mem->alloc_sarray)
        ((j_common_ptr)&cinfo, JPOOL_IMAGE, (size_t)row_stride, 1);

    size_t offset = 0;
    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, buffer, 1);
        memcpy(pixels + offset, buffer[0], row_stride);
        offset += row_stride;
    }

    jpeg_finish_decompress(&cinfo);

    *out_pixels = pixels;
    *out_len = pixels_size;
    *width = (int)cinfo.output_width;
    *height = (int)cinfo.output_height;
    *channels = cinfo.output_components;
    ret = 0;

cleanup:
    jpeg_destroy_decompress(&cinfo);
    return ret;
}

/* Encode RGB to JPEG in memory (no alpha support). */
int jpeg_encode_image(const uint8_t* pixels, size_t pixels_len,
                      int width, int height, int channels,
                      uint8_t** out_buf, size_t* out_buf_len) {
    if (!pixels || width <= 0 || height <= 0 || channels != 3) return -1;
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    unsigned char* mem = NULL; unsigned long memsize = 0;
    JSAMPROW row_pointer[1];
    int quality = 90;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &mem, &memsize);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = channels;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    int row_stride = width * channels;
    while (cinfo.next_scanline < cinfo.image_height) {
        row_pointer[0] = (JSAMPROW)(pixels + cinfo.next_scanline * row_stride);
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }
    jpeg_finish_compress(&cinfo);

    *out_buf = (uint8_t*)mem; /* ownership transferred */
    *out_buf_len = (size_t)memsize;
    jpeg_destroy_compress(&cinfo);
    return 0;
}