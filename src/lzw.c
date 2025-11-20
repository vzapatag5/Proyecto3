// =============================================================================
// lzw.c - Implementación sencilla de LZW (12 bits) para compresión de bytes
// =============================================================================
// Idea general (compresión):
//   - Empezamos con un diccionario inicial que contiene todas las secuencias
//     de 1 byte (códigos 0..255).
//   - Leemos la entrada, buscamos la secuencia más larga ya conocida y cuando
//     la siguiente combinación no está en el diccionario, emitimos el código de
//     la secuencia actual y añadimos la nueva secuencia al diccionario.
//   - Los códigos se guardan usando 12 bits.
//
// Descompresión:
//   - Reconstruimos las secuencias a partir de los códigos recibidos.
//   - Vamos rellenando el diccionario de la misma forma que en compresión.
//   - Caso especial: cuando aparece un código que justo es el siguiente a
//     asignar (aún no existe), se reconstruye usando la regla KwKwK.
//
// Limitaciones / Simplificaciones:
//   - Tamaño máximo del diccionario: 4096 entradas (12 bits).
//   - No se implementan códigos especiales de reset/clear.
//   - El empaquetado de bits es LSB-first (se guardan los bits menos
//     significativos primero). Esto es una decisión de implementación.
// =============================================================================

#include "lzw.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Notas rápidas de implementación:
 * - Códigos de 12 bits (0..4095). Diccionario inicial: 0..255 (bytes sueltos).
 * - Diccionario máximo: 4096 entradas.
 * - Para buscar la siguiente secuencia se usa una tabla tipo trie: next[code][byte].
 * - Los códigos se empaquetan en un flujo de bits (12 bits por código).
 */

#define LZW_MAX_CODES 4096
#define LZW_BIT_WIDTH 12

/* ayudas para escritura en memoria */
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

/* Bitstream writer para códigos de 12 bits */
typedef struct { mem_writer mw; uint32_t bitbuf; int bitcount; } bit_writer;
static int bw_init(bit_writer* bw) { bw->bitbuf = 0; bw->bitcount = 0; return mw_init(&bw->mw); }
static void bw_free(bit_writer* bw) { mw_free(&bw->mw); }
static int bw_write(bit_writer* bw, uint32_t code) {
    /* escribe LZW_BIT_WIDTH bits LSB-first en el buffer */
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

    // Tabla "next": para cada código existente y posible siguiente byte
    // guarda el nuevo código que representa la secuencia extendida.
    int *next = (int*)malloc(sizeof(int) * LZW_MAX_CODES * 256);
    if (!next) return -1;
    for (int i=0;i<LZW_MAX_CODES*256;i++) next[i] = -1; // -1 indica "no existe aún"

    bit_writer bw; bw_init(&bw);

    int next_code = 256; // siguiente código libre (los 0..255 ya están implícitos)

    // Comenzamos con el primer byte como secuencia actual
    int code = in[0];
    for (size_t i=1;i<in_len;i++) {
        uint8_t c = in[i];
        int idx = code * 256 + c; // posición en la tabla para (secuencia_actual + nuevo_byte)
        int nc = next[idx];
        if (nc != -1) {
            // La secuencia extendida ya existe: la adoptamos como nueva secuencia actual
            code = nc;
        } else {
            // No existe: emitimos el código de la secuencia actual
            if (bw_write(&bw, (uint32_t)code) != 0) { free(next); bw_free(&bw); return -1; }
            // Añadimos la nueva secuencia si aún hay espacio en el diccionario
            if (next_code < LZW_MAX_CODES) {
                next[idx] = next_code++;
            }
            // Empezamos nueva secuencia con el byte actual
            code = c;
        }
    }
    // Emitir el último código pendiente
    if (bw_write(&bw, (uint32_t)code) != 0) { free(next); bw_free(&bw); return -1; }
    // Vaciar cualquier resto de bits en el buffer
    if (bw_flush(&bw) != 0) { free(next); bw_free(&bw); return -1; }

    // Copiar resultado al buffer de salida
    *out_len = bw.mw.size;
    *out = (uint8_t*)malloc(*out_len ? *out_len : 1);
    if (!*out) { free(next); bw_free(&bw); return -1; }
    memcpy(*out, bw.mw.buf, *out_len);

    free(next);
    bw_free(&bw);
    return 0;
}

/* Bitstream reader para códigos de 12 bits */
typedef struct { const uint8_t* buf; size_t size; size_t pos; uint32_t bitbuf; int bitcount; } bit_reader;
static void br_init(bit_reader* br, const uint8_t* buf, size_t size) { br->buf = buf; br->size = size; br->pos = 0; br->bitbuf = 0; br->bitcount = 0; }
static int br_read(bit_reader* br, uint32_t* out_code) {
    while (br->bitcount < LZW_BIT_WIDTH) {
        if (br->pos >= br->size) break;
        br->bitbuf |= ((uint32_t)br->buf[br->pos++]) << br->bitcount;
        br->bitcount += 8;
    }
    if (br->bitcount < LZW_BIT_WIDTH) return 0; /* no mas full codes */
    *out_code = br->bitbuf & ((1u<<LZW_BIT_WIDTH)-1);
    br->bitbuf >>= LZW_BIT_WIDTH;
    br->bitcount -= LZW_BIT_WIDTH;
    return 1;
}

int lzw_decompress(const uint8_t* in, size_t in_len, uint8_t** out, size_t* out_len) {
    if (!in || in_len == 0 || !out || !out_len) return -1;

    // Diccionario representado por: prefix[] y suffix[]
    // Cada código representa una secuencia: se reconstruye caminando hacia atrás
    // usando prefix hasta llegar a -1, recolectando los suffix.
    int *prefix = (int*)malloc(sizeof(int) * LZW_MAX_CODES);
    unsigned char *suffix = (unsigned char*)malloc(sizeof(unsigned char) * LZW_MAX_CODES);
    if (!prefix || !suffix) { free(prefix); free(suffix); return -1; }

    // Inicializar códigos base (0..255): secuencias de un solo byte
    for (int i=0;i<256;i++) { prefix[i] = -1; suffix[i] = (unsigned char)i; }
    int next_code = 256;

    mem_writer mw; if (mw_init(&mw)!=0) { free(prefix); free(suffix); return -1; }

    bit_reader br; br_init(&br, in, in_len);
    uint32_t code_u;
    if (!br_read(&br, &code_u)) { mw_free(&mw); free(prefix); free(suffix); return -1; }
    int old_code = (int)code_u;

    // Primer código debe ser un byte literal (<256)
    if (old_code < 0 || old_code >= LZW_MAX_CODES) { mw_free(&mw); free(prefix); free(suffix); return -1; }
    if (old_code < 256) {
        if (mw_ensure(&mw,1)!=0) { mw_free(&mw); free(prefix); free(suffix); return -1; }
        mw.buf[mw.size++] = (uint8_t)old_code;
    } else {
        // No debería ocurrir
        mw_free(&mw); free(prefix); free(suffix); return -1;
    }

    uint8_t decode_stack[4096]; // pila temporal para reconstruir secuencias

    while (br_read(&br, &code_u)) {
        int code = (int)code_u;
        int in_code = code; // recordar el código original leído
        int stack_top = 0;

        if (code < next_code) {
            // Código existente: reconstruir secuencia subiendo por prefix
            int cur = code;
            while (cur != -1) {
                decode_stack[stack_top++] = suffix[cur];
                cur = prefix[cur];
            }
        } else if (code == next_code) {
            // Caso especial (KwKwK): secuencia = anterior + primer carácter de la anterior
            int cur = old_code;
            while (cur != -1) { decode_stack[stack_top++] = suffix[cur]; cur = prefix[cur]; }
            if (stack_top == 0) { mw_free(&mw); free(prefix); free(suffix); return -1; }
            unsigned char first_char = decode_stack[stack_top-1];
            decode_stack[stack_top++] = first_char;
        } else {
            // Código inválido
            mw_free(&mw); free(prefix); free(suffix); return -1;
        }

        // Escribir la secuencia decodificada en orden correcto (invertimos la pila)
        for (int i = stack_top-1; i >= 0; --i) {
            if (mw_ensure(&mw,1)!=0) { mw_free(&mw); free(prefix); free(suffix); return -1; }
            mw.buf[mw.size++] = decode_stack[i];
        }

        // Añadir nueva entrada al diccionario: old_code + primer carácter de la secuencia actual
        if (next_code < LZW_MAX_CODES) {
            unsigned char first_char = decode_stack[stack_top-1];
            prefix[next_code] = old_code;
            suffix[next_code] = first_char;
            next_code++;
        }

        old_code = in_code; // avanzar
    }

    *out_len = mw.size;
    *out = (uint8_t*)malloc(*out_len ? *out_len : 1);
    if (!*out) { mw_free(&mw); free(prefix); free(suffix); return -1; }
    memcpy(*out, mw.buf, *out_len);

    mw_free(&mw);
    free(prefix); free(suffix);
    return 0;
}
