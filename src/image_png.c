#include "image_png.h"
#include <png.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

typedef struct { const uint8_t* data; size_t size; size_t offset; } mem_reader;
static void png_mem_read(png_structp png_ptr, png_bytep out, png_size_t count) {
    mem_reader* r = (mem_reader*)png_get_io_ptr(png_ptr);
    if (r->offset + count > r->size) png_error(png_ptr, "Read past end");
    memcpy(out, r->data + r->offset, count);
    r->offset += count;
}

typedef struct { uint8_t* buf; size_t size; size_t cap; } mem_writer;
static void png_mem_write(png_structp png_ptr, png_bytep data, png_size_t length) {
    mem_writer* w = (mem_writer*)png_get_io_ptr(png_ptr);
    if (w->size + length > w->cap) {
        size_t nc = (w->cap ? w->cap*2 : 4096);
        while (nc < w->size + length) nc *= 2;
        uint8_t* tmp = (uint8_t*)realloc(w->buf, nc);
        if (!tmp) png_error(png_ptr, "Out of memory");
        w->buf = tmp; w->cap = nc;
    }
    memcpy(w->buf + w->size, data, length);
    w->size += length;
}
static void png_mem_flush(png_structp png_ptr) { (void)png_ptr; }

/* Decode PNG into RGBA (channels out will be 4) */
/* custom libpng error handler that jumps instead of printing to stderr */
static void png_silent_error(png_structp png_ptr, png_const_charp msg) {
    (void)msg;
    longjmp(png_jmpbuf(png_ptr), 1);
}
static void png_silent_warn(png_structp png_ptr, png_const_charp msg) { (void)png_ptr; (void)msg; }

int png_decode_image(const uint8_t* in_buf, size_t in_len,
                     uint8_t** out_pixels, size_t* out_len,
                     int* width, int* height, int* channels) {
    if (!in_buf || in_len < 8) return -1;
    png_structp png_ptr = NULL;
    png_infop info_ptr = NULL;
    mem_reader reader = { in_buf, in_len, 0 };
    int ret = -1;
    png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, png_silent_error, png_silent_warn);
    if (!png_ptr) return -1;
    info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) { png_destroy_read_struct(&png_ptr, NULL, NULL); return -1; }
    if (setjmp(png_jmpbuf(png_ptr))) { goto cleanup; }

    png_set_read_fn(png_ptr, &reader, png_mem_read);
    png_read_info(png_ptr, info_ptr);

    png_uint_32 w = 0, h = 0;
    int color_type = 0, bit_depth = 0;
    png_get_IHDR(png_ptr, info_ptr, &w, &h, &bit_depth, &color_type, NULL, NULL, NULL);

    if (bit_depth == 16) png_set_strip_16(png_ptr);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png_ptr);
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png_ptr);

    png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER); /* force alpha */
    png_read_update_info(png_ptr, info_ptr);

    png_size_t rowbytes = png_get_rowbytes(png_ptr, info_ptr);
    size_t pixels_size = (size_t)rowbytes * h;
    uint8_t* pixels = NULL;
    png_bytep* rows = NULL;

    pixels = (uint8_t*)malloc(pixels_size ? pixels_size : 1);
    if (!pixels) goto cleanup;

    rows = (png_bytep*)malloc(sizeof(png_bytep) * h);
    if (!rows) { goto cleanup; }
    for (png_uint_32 y = 0; y < h; ++y) rows[y] = pixels + y * rowbytes;

    png_read_image(png_ptr, rows);
    png_read_end(png_ptr, NULL);

    *out_pixels = pixels;
    *out_len = pixels_size;
    *width = (int)w;
    *height = (int)h;
    *channels = 4;
    ret = 0;

cleanup:
    if (rows) free(rows);
    if (ret != 0) { if (pixels) free(pixels); }
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    return ret;
}

/* Encode RGBA/RGB to PNG in memory */
int png_encode_image(const uint8_t* pixels, size_t pixels_len,
                     int width, int height, int channels,
                     uint8_t** out_buf, size_t* out_buf_len) {
    if (!pixels || width <= 0 || height <= 0 || (channels != 3 && channels != 4)) return -1;

    png_structp png_ptr = NULL;
    png_infop info_ptr = NULL;
    mem_writer writer = { NULL, 0, 0 };
    int ret = -1;

    png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) return -1;
    info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) { png_destroy_write_struct(&png_ptr, NULL); return -1; }
    if (setjmp(png_jmpbuf(png_ptr))) { goto cleanup; }

    png_set_write_fn(png_ptr, &writer, png_mem_write, png_mem_flush);

    int color_type = (channels == 4) ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB;
    png_set_IHDR(png_ptr, info_ptr, (png_uint_32)width, (png_uint_32)height,
                 8, color_type, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png_ptr, info_ptr);

    png_bytep* rows = (png_bytep*)malloc(sizeof(png_bytep) * height);
    if (!rows) goto cleanup;
    png_size_t rowbytes = (png_size_t)width * channels;
    for (int y = 0; y < height; ++y) rows[y] = (png_bytep)(pixels + (size_t)y * rowbytes);

    png_write_image(png_ptr, rows);
    png_write_end(png_ptr, info_ptr);

    *out_buf = writer.buf;
    *out_buf_len = writer.size;
    writer.buf = NULL; /* transfer ownership */
    ret = 0;

    free(rows);
cleanup:
    if (writer.buf) free(writer.buf);
    png_destroy_write_struct(&png_ptr, &info_ptr);
    return ret;
}