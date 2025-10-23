#ifndef XOR_H
#define XOR_H
#include <stddef.h>
#include <stdint.h>

void xor_apply(uint8_t* buf, size_t len, const uint8_t* key, size_t key_len);

#endif
