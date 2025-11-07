#include "lzw.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Implementation notes:
 * - Uses 12-bit codes (0..4095). Initial dictionary: 0..255 single bytes.
 * - Dictionary max size: 4096.
 * - Compression uses a trie-like next[4096][256] table for fast lookup.
 * - Output codes are packed 12 bits LSB-first into bytes.
 */

#define LZW_MAX_CODES 4096
#define LZW_BIT_WIDTH 12

/* Memory writer helper */
typedef struct { uint8_t* buf; size_t size; size_t cap; } mem_writer;
static int mw_init(mem_writer* w) { w->buf = NULL; w->size = w->cap = 0; return 0; }
static void mw_free(mem_writer* w) { if (w->buf) free(w->buf); w->buf = NULL; w->size = w->cap = 0; }
static int mw_ensure(mem_writer* w, size_t need) {
    if (w->size + need <= w->cap) return 0;
    size_t nc = w->cap ? w->cap * 2 : 4096;
    while (nc < w->size + need) nc *= 2;
    uint8_t* tmp = (uint8_t*)realloc(w->buf, nc);
    if (!tmp) return -1;
    w->buf = tmp; w->cap = nc; return 0;
}
static int mw_write_u8(mem_writer* w, uint8_t b) { if (mw_ensure(w,1)!=0) return -1; w->buf[w->size++]=b; return 0; }

/* Bitstream writer for 12-bit codes */
typedef struct { mem_writer mw; uint32_t bitbuf; int bitcount; } bit_writer;
static int bw_init(bit_writer* bw) { bw->bitbuf = 0; bw->bitcount = 0; return mw_init(&bw->mw); }
static void bw_free(bit_writer* bw) { mw_free(&bw->mw); }
static int bw_write(bit_writer* bw, uint32_t code) {
    /* write LZW_BIT_WIDTH bits LSB-first into buffer */
    bw->bitbuf |= (code & ((1u<<LZW_BIT_WIDTH)-1)) << bw->bitcount;
    bw->bitcount += LZW_BIT_WIDTH;
    while (bw->bitcount >= 8) {
        if (mw_write_u8(&bw->mw, (uint8_t)(bw->bitbuf & 0xFF)) != 0) return -1;
        bw->bitbuf >>= 8; bw->bitcount -= 8;
    }
    return 0;
}
static int bw_flush(bit_writer* bw) {
    while (bw->bitcount > 0) {
        if (mw_write_u8(&bw->mw, (uint8_t)(bw->bitbuf & 0xFF)) != 0) return -1;
        bw->bitbuf >>= 8; bw->bitcount -= 8;
    }
    return 0;
}

int lzw_compress(const uint8_t* in, size_t in_len, uint8_t** out, size_t* out_len) {
    if (!in || in_len == 0 || !out || !out_len) return -1;

    

    /* allocate next table: LZW_MAX_CODES x 256 ints (int32) */
    /* To keep memory moderate we allocate as a flat array */
    int *next = (int*)malloc(sizeof(int) * LZW_MAX_CODES * 256);
    if (!next) return -1;
    for (int i=0;i<LZW_MAX_CODES*256;i++) next[i] = -1;

    bit_writer bw; bw_init(&bw);

    /* Initialize: codes 0..255 exist implicitly as single-byte sequences. next[] indexes by [code*256 + byte] */
    int next_code = 256;

    /* start with first byte */
    int code = in[0];
    for (size_t i=1;i<in_len;i++) {
        uint8_t c = in[i];
        int idx = code * 256 + c;
        int nc = next[idx];
        if (nc != -1) {
            code = nc;
        } else {
            /* output code */
            if (bw_write(&bw, (uint32_t)code) != 0) { free(next); bw_free(&bw); return -1; }
            /* add new entry if room */
            if (next_code < LZW_MAX_CODES) {
                next[idx] = next_code++;
            }
            code = c; /* start new sequence */
        }
    }
    /* output last code */
    if (bw_write(&bw, (uint32_t)code) != 0) { free(next); bw_free(&bw); return -1; }
    if (bw_flush(&bw) != 0) { free(next); bw_free(&bw); return -1; }

    /* transfer out */
    *out_len = bw.mw.size;
    *out = (uint8_t*)malloc(*out_len ? *out_len : 1);
    if (!*out) { free(next); bw_free(&bw); return -1; }
    memcpy(*out, bw.mw.buf, *out_len);

    free(next);
    bw_free(&bw);
    return 0;
}

/* Bitstream reader for 12-bit codes */
typedef struct { const uint8_t* buf; size_t size; size_t pos; uint32_t bitbuf; int bitcount; } bit_reader;
static void br_init(bit_reader* br, const uint8_t* buf, size_t size) { br->buf = buf; br->size = size; br->pos = 0; br->bitbuf = 0; br->bitcount = 0; }
static int br_read(bit_reader* br, uint32_t* out_code) {
    while (br->bitcount < LZW_BIT_WIDTH) {
        if (br->pos >= br->size) break;
        br->bitbuf |= ((uint32_t)br->buf[br->pos++]) << br->bitcount;
        br->bitcount += 8;
    }
    if (br->bitcount < LZW_BIT_WIDTH) return 0; /* no more full codes */
    *out_code = br->bitbuf & ((1u<<LZW_BIT_WIDTH)-1);
    br->bitbuf >>= LZW_BIT_WIDTH;
    br->bitcount -= LZW_BIT_WIDTH;
    return 1;
}

int lzw_decompress(const uint8_t* in, size_t in_len, uint8_t** out, size_t* out_len) {
    if (!in || in_len == 0 || !out || !out_len) return -1;


    /* tables */
    int *prefix = (int*)malloc(sizeof(int) * LZW_MAX_CODES);
    unsigned char *suffix = (unsigned char*)malloc(sizeof(unsigned char) * LZW_MAX_CODES);
    if (!prefix || !suffix) { free(prefix); free(suffix); return -1; }

    /* initialize dictionary 0..255 */
    for (int i=0;i<256;i++) { prefix[i] = -1; suffix[i] = (unsigned char)i; }
    int next_code = 256;

    /* output buffer writer */
    mem_writer mw; if (mw_init(&mw)!=0) { free(prefix); free(suffix); return -1; }

    bit_reader br; br_init(&br, in, in_len);
    uint32_t code_u;
    if (!br_read(&br, &code_u)) { mw_free(&mw); free(prefix); free(suffix); return -1; }
    int old_code = (int)code_u;
    
    /* output old_code string */
    if (old_code < 0 || old_code >= LZW_MAX_CODES) { mw_free(&mw); free(prefix); free(suffix); return -1; }
    if (old_code < 256) { if (mw_ensure(&mw,1)!=0) { mw_free(&mw); free(prefix); free(suffix); return -1; } mw.buf[mw.size++] = (uint8_t)old_code; }
    else {
        /* should not happen as first code must be <256 */
        mw_free(&mw); free(prefix); free(suffix); return -1;
    }

    uint8_t decode_stack[4096];

    while (br_read(&br, &code_u)) {
        int code = (int)code_u;
        
        int in_code = code;
        int stack_top = 0;

        if (code < next_code) {
            /* decode code into stack */
            int cur = code;
            while (cur != -1) {
                decode_stack[stack_top++] = suffix[cur];
                cur = prefix[cur];
            }
        } else if (code == next_code) {
            /* special KwKwK case: string = old_string + first_char(old_string) */
            int cur = old_code;
            while (cur != -1) { decode_stack[stack_top++] = suffix[cur]; cur = prefix[cur]; }
            /* first char is last element of stack */
            if (stack_top == 0) { mw_free(&mw); free(prefix); free(suffix); return -1; }
            unsigned char first_char = decode_stack[stack_top-1];
            decode_stack[stack_top++] = first_char;
        } else {
            /* invalid code */
            mw_free(&mw); free(prefix); free(suffix); return -1;
        }

        /* push decoded string to output in reverse order */
        for (int i = stack_top-1; i >= 0; --i) {
            if (mw_ensure(&mw,1)!=0) { mw_free(&mw); free(prefix); free(suffix); return -1; }
            mw.buf[mw.size++] = decode_stack[i];
        }

        /* add new dictionary entry: prefix = old_code, suffix = first char of decoded string */
        if (next_code < LZW_MAX_CODES) {
            /* first char of decoded string is decode_stack[stack_top-1] */
            unsigned char first_char = decode_stack[stack_top-1];
            prefix[next_code] = old_code;
            suffix[next_code] = first_char;
            next_code++;
        }

        old_code = in_code;
    }

    *out_len = mw.size;
    *out = (uint8_t*)malloc(*out_len ? *out_len : 1);
    if (!*out) { mw_free(&mw); free(prefix); free(suffix); return -1; }
    memcpy(*out, mw.buf, *out_len);

    mw_free(&mw);
    free(prefix); free(suffix);
    return 0;
}
