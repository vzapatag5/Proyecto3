#include "rle.h"
#include <stdlib.h>

int rle_compress(const uint8_t* in, size_t in_len, uint8_t** out, size_t* out_len) {
    *out = NULL; *out_len = 0;
    if (in_len == 0) {
        *out = (uint8_t*)malloc(1); if(!*out) return -1;
        *out_len = 0; return 0;
    }
    size_t cap = in_len * 2 + 2;
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (!buf) return -1;

    size_t i = 0, k = 0;
    while (i < in_len) {
        uint8_t b = in[i];
        size_t run = 1;
        while (i + run < in_len && in[i + run] == b && run < 255) run++;
        buf[k++] = (uint8_t)run;
        buf[k++] = b;
        i += run;
    }
    *out = buf;
    *out_len = k;
    return 0;
}

int rle_decompress(const uint8_t* in, size_t in_len, uint8_t** out, size_t* out_len) {
    *out = NULL; *out_len = 0;
    if (in_len == 0) {
        *out = (uint8_t*)malloc(1); if(!*out) return -1;
        *out_len = 0; return 0;
    }
    size_t cap = in_len * 255;
    if (cap < in_len) cap = in_len;
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (!buf) return -1;

    size_t i = 0, k = 0;
    while (i + 1 < in_len) {
        uint8_t count = in[i++];
        uint8_t val   = in[i++];
        for (int c = 0; c < count; ++c) buf[k++] = val;
    }
    uint8_t* shrink = (uint8_t*)realloc(buf, k ? k : 1);
    if (shrink) buf = shrink;
    *out = buf;
    *out_len = k;
    return 0;
}
