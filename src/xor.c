#include "xor.h"

void xor_apply(uint8_t* buf, size_t len, const uint8_t* key, size_t key_len) {
    if (key_len == 0) return;
    for (size_t i = 0; i < len; ++i) {
        buf[i] ^= key[i % key_len];
    }
}
