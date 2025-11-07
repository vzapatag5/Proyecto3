#ifndef LZW_H
#define LZW_H

#include <stddef.h>
#include <stdint.h>

/* Simple LZW (12-bit codes) compressor/decompressor.
 * API:
 *  - lzw_compress: compress input buffer, returns malloc'd output in *out (caller frees)
 *  - lzw_decompress: decompress input buffer, returns malloc'd output in *out (caller frees)
 * Returns 0 on success, -1 on failure.
 */
int lzw_compress(const uint8_t* in, size_t in_len, uint8_t** out, size_t* out_len);
int lzw_decompress(const uint8_t* in, size_t in_len, uint8_t** out, size_t* out_len);

#endif
