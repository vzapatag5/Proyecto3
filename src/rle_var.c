/* =============================================================
 * RLE_VAR - Variante simple de Run-Length Encoding
 * -------------------------------------------------------------
 * Formato de salida (secuencia de bloques):
 *   - Bloque LITERAL: encabezado (1..127) = cantidad de bytes crudos, seguido
 *     de esos 'n' bytes tal cual.
 *   - Bloque RUN: encabezado con bit alto (0x80) y los 7 bits restantes =
 *     longitud (3..127), seguido de 1 byte con el valor repetido.
 * Reglas:
 *   - Solo generamos runs de longitud >=3 (2 repetidos no vale la pena).
 *   - Longitud máxima codificada: 127 (para caber en 7 bits).
 *   - Los literales se agrupan hasta antes de un run largo o hasta 127 bytes.
 * Objetivo educativo: mostrar un formato compacto y fácil de decodificar.
 * ============================================================= */
#include "rle_var.h"
#include <stdlib.h>
#include <string.h>

/* emit_literal: escribe un bloque literal (n bytes crudos) con encabezado 'n'. */
static void emit_literal(uint8_t** out, size_t* k, size_t* cap, const uint8_t* lit, size_t n) {
    if (*k + 1 + n > *cap) {
        size_t ncap = (*cap) * 2 + 1 + n;
        uint8_t* tmp = (uint8_t*)realloc(*out, ncap);
        if (!tmp) return;
        *out = tmp; *cap = ncap;
    }
    (*out)[(*k)++] = (uint8_t)n;         // len (1..127)
    memcpy(*out + *k, lit, n); *k += n;  // copia literales
}

/* emit_run: escribe un bloque de repetición (run) con marca 0x80 | len. */
static void emit_run(uint8_t** out, size_t* k, size_t* cap, uint8_t val, size_t run_len) {
    if (run_len < 3) return; // contrato del llamador
    if (run_len > 127) run_len = 127;
    if (*k + 2 > *cap) {
        size_t ncap = (*cap) * 2 + 2;
        uint8_t* tmp = (uint8_t*)realloc(*out, ncap);
        if (!tmp) return;
        *out = tmp; *cap = ncap;
    }
    (*out)[(*k)++] = (uint8_t)(0x80 | (uint8_t)run_len); // marca de run
    (*out)[(*k)++] = val;
}

/* rle_var_compress:
 * Recorre la entrada detectando runs >=3; si no hay run largo, acumula
 * bytes en un bloque literal. Al final ajusta el buffer al tamaño exacto.
 */
int rle_var_compress(const uint8_t* in, size_t in_len, uint8_t** out, size_t* out_len) {
    *out = NULL; *out_len = 0;
    if (in_len == 0) {
        *out = (uint8_t*)malloc(1); if (!*out) return -1;
        *out_len = 0; return 0;
    }

    size_t cap = in_len + in_len/8 + 16; // heurística
    uint8_t* buf = (uint8_t*)malloc(cap ? cap : 1);
    if (!buf) return -1;

    size_t i = 0, k = 0;
    while (i < in_len) {
        /* Detectar run a partir de posición 'i' */
        size_t run = 1;
        while (i + run < in_len && in[i + run] == in[i] && run < 127) run++;

        if (run >= 3) {
            emit_run(&buf, &k, &cap, in[i], run);
            i += run;
        } else {
            /* Construir bloque literal hasta antes de un run largo o llegar a 127 */
            uint8_t lit[127];
            size_t n = 0;
            lit[n++] = in[i++]; // primer byte
            /* Añadir más bytes mientras no empiece un run largo */
            while (i < in_len && n < 127) {
                /* ¿Se forma run >=3 desde i? Si sí, paramos para que lo maneje el siguiente ciclo */
                size_t r = 1;
                while (i + r < in_len && in[i + r] == in[i] && r < 127) r++;
                if (r >= 3) break; // nos detenemos antes del run largo
                lit[n++] = in[i++];
            }
            emit_literal(&buf, &k, &cap, lit, n);
        }
    }

    uint8_t* shrink = (uint8_t*)realloc(buf, k ? k : 1);
    if (shrink) buf = shrink;
    *out = buf; *out_len = k;
    return 0;
}

/* rle_var_decompress:
 * Lee bloque por bloque: si el bit alto del encabezado está activo es un run;
 * si no, es un literal. Primero hace una pasada para estimar tamaño final.
 */
int rle_var_decompress(const uint8_t* in, size_t in_len, uint8_t** out, size_t* out_len) {
    *out = NULL; *out_len = 0;
    if (in_len == 0) {
        uint8_t* empty = (uint8_t*)malloc(1);
        if (!empty) return -1;
        *out = empty; *out_len = 0; return 0;
    }

    /* Primera pasada: estimar tamaño de salida para reservar memoria */
    size_t est = 0, i = 0;
    while (i < in_len) {
        uint8_t hdr = in[i++];
        if (hdr & 0x80) { /* run */
            if (i >= in_len) return -1;
            uint8_t run_len = (uint8_t)(hdr & 0x7F);
            i += 1; // salta el byte de valor
            est += run_len;
        } else { /* literal */
            uint8_t n = hdr;
            if (i + n > in_len) return -1;
            i += n;
            est += n;
        }
    }
    if (est == 0) est = 1;

    uint8_t* buf = (uint8_t*)malloc(est);
    if (!buf) return -1;

    size_t k = 0; i = 0;
    while (i < in_len) {
        uint8_t hdr = in[i++];
        if (hdr & 0x80) { /* run */
            if (i >= in_len) { free(buf); return -1; }
            uint8_t run_len = (uint8_t)(hdr & 0x7F);
            uint8_t val = in[i++];
            for (uint8_t c = 0; c < run_len; ++c) buf[k++] = val;
        } else { /* literal */
            uint8_t n = hdr;
            if (i + n > in_len) { free(buf); return -1; }
            memcpy(buf + k, in + i, n);
            k += n; i += n;
        }
    }
    uint8_t* shrink = (uint8_t*)realloc(buf, k ? k : 1);
    if (shrink) buf = shrink;
    *out = buf; *out_len = k;
    return 0;
}
