#ifndef IMAGE_PNG_H
#define IMAGE_PNG_H
#include <stddef.h>
#include <stdint.h>

/* Decodifica PNG desde memoria -> pixels (RGBA 8bpp).
   Devuelve 0 en éxito. `*out_pixels` es malloc y debe free(). */
int png_decode_image(const uint8_t* in_buf, size_t in_len,
                     uint8_t** out_pixels, size_t* out_len,
                     int* width, int* height, int* channels);

/* Codifica PNG (pixels RGBA/RGB) a memoria. Devuelve 0 en éxito.
   `*out_buf` es malloc y debe free(). */
int png_encode_image(const uint8_t* pixels, size_t pixels_len,
                     int width, int height, int channels,
                     uint8_t** out_buf, size_t* out_buf_len);

#endif