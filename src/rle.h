#ifndef RLE_H
#define RLE_H
#include <stddef.h>
#include <stdint.h>

int rle_compress(const uint8_t* in, size_t in_len, uint8_t** out, size_t* out_len);
int rle_decompress(const uint8_t* in, size_t in_len, uint8_t** out, size_t* out_len);

#endif
