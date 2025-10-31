#ifndef RLE_VAR_H
#define RLE_VAR_H
#include <stddef.h>
#include <stdint.h>

// RLE con literales (tipo PackBits reducido)
// Bloque literal: [len (1..127)] [len bytes]
// Bloque run:     [0x80 | run_len (3..127)] [byte]

int rle_var_compress(const uint8_t* in, size_t in_len, uint8_t** out, size_t* out_len);
int rle_var_decompress(const uint8_t* in, size_t in_len, uint8_t** out, size_t* out_len);

#endif
