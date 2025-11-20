

/* =============================================================
 * GSEA - Compresión y Encriptación
 * -------------------------------------------------------------
 * Este archivo coordina el flujo:
 *   1. Lee parámetros (-c -d -e -u, algoritmos, hilos, chunk).
 *   2. Procesa un archivo o todos los de una carpeta.
 *   3. Etapas opcionales: compresión / cifrado / descifrado / descompresión.
 *   4. Soporte especial para WAV (delta16) y predictor SUB en algunos modos.
 *   5. Paralelismo externo (archivos) e interno (chunks grandes).
 *   6. Journaling (-j) para ver pasos en tiempo real.
 * Objetivo: ofrecer un pipeline claro y modular para estudiar.
 * ============================================================= */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <dirent.h>
#include <pthread.h>
#include <time.h>

#include "fs.h"
#include "rle_var.h"
#include "lzw.h"
#include "vigenere.h"
#include "image_png.h"
#include "image_jpeg.h"
#include "huffman_predictor.h"
#include "aes_simple.h"
#include "audio_wav.h"
#include "thread_pool.h"
#include "journal.h"  

/* Constantes generales */
#define WAV_MAGIC       "GSEAWAV1"
#define WAV_MAGIC_LEN   8


/* Tamaño por defecto de chunk para procesamiento en paralelo: 100 MB */
#define DEFAULT_CHUNK_MB 100

/* Algoritmos de compresión disponibles */
typedef enum {
    COMP_RLEVAR,
    COMP_LZW,
    COMP_LZWPRED,
    COMP_HUFFMANPRED,
    COMP_DELTA16_LZW,
    COMP_DELTA16_HUFF
} CompAlg;

/* Algoritmos de encriptación disponibles */
typedef enum {
    ENC_NONE,
    ENC_VIG,
    ENC_AES
} EncAlg;

/* Config: opciones elegidas por el usuario. Campos clave:
 * do_c/do_d/do_e/do_u = qué operaciones se activan.
 * comp_alg / enc_alg  = algoritmos seleccionados.
 * workers / inner_workers = hilos externos (archivos) e internos (chunks). 0=auto.
 * chunk_bytes = tamaño de cada porción para dividir archivos grandes.
 * journal = controla si se imprimen mensajes paso a paso.
 */
typedef struct {
    int do_c, do_d, do_e, do_u;
    const char* in_path;
    const char* out_path;
    const char* key;
    CompAlg comp_alg;
    EncAlg  enc_alg;

    int workers;
    int inner_workers;
    int verbose;

    size_t chunk_bytes;   

    Journal journal;
} Config;

/* Prototipos principales del pipeline */
static int   run_interactive(void);
static void  human_readable(size_t bytes, char* out, size_t out_size);

/* Predictor SUB: resta el pixel/byte anterior para generar diferencias */
static void apply_predictor_sub(uint8_t* buf, int w, int h, int ch);
static void undo_predictor_sub(uint8_t* buf, int w, int h, int ch);


static void delta16_forward(int16_t* s, size_t frames, int ch);  /* Aplica diferencia entre muestras PCM16 */
static void delta16_inverse(int16_t* s, size_t frames, int ch);  /* Revierte la diferencia */


static void wr16le(uint8_t* p, uint16_t v); /* Escribe 16 bits little-endian */
static void wr32le(uint8_t* p, uint32_t v); /* Escribe 32 bits little-endian */
static uint16_t rd16le(const uint8_t* p);  /* Lee 16 bits little-endian */
static uint32_t rd32le(const uint8_t* p);  /* Lee 32 bits little-endian */

/* Pipeline principal */
static int process_one_file(const char* in, const char* out, const Config* cfg,
                            size_t* o_orig, size_t* o_fin, double* o_ms); /* Ejecuta todas las etapas sobre 1 archivo */
static int compress_chunked(const Config* cfg,
                            const uint8_t* in, size_t in_len,
                            uint8_t** out, size_t* out_len);        /* Divide y comprime por trozos */
static int decompress_chunked(const Config* cfg,
                              const uint8_t* in, size_t in_len,
                              uint8_t** out, size_t* out_len);      /* Reconstruye concatenando trozos */


static int hw_threads(void); /* Detecta núcleos disponibles */
static int compress_chunked_parallel(const Config* cfg,
                                     const uint8_t* in, size_t in_len,
                                     uint8_t** out, size_t* out_len); /* Versión paralela interna */




static void apply_predictor_sub(uint8_t* buf, int w, int h, int ch) {
    /* Recorre la fila y reemplaza cada valor por (actual - anterior) */
    if (!buf) return;
    for (int y = 0; y < h; ++y) {
        size_t base = (size_t)y * w * ch;
        uint8_t left[4] = {0,0,0,0};
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < ch; ++c) {
                size_t i = base + x*ch + c;
                uint8_t cur = buf[i];
                uint8_t pred = (uint8_t)(cur - left[c]);
                buf[i] = pred;
                left[c] = cur;
            }
        }
    }
}

static void undo_predictor_sub(uint8_t* buf, int w, int h, int ch) {
    /* Reconstruye sumando el valor previo */
    if (!buf) return;
    for (int y = 0; y < h; ++y) {
        size_t base = (size_t)y * w * ch;
        uint8_t left[4] = {0,0,0,0};
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < ch; ++c) {
                size_t i = base + x*ch + c;
                uint8_t pred = buf[i];
                uint8_t cur = (uint8_t)(pred + left[c]);
                buf[i] = cur;
                left[c] = cur;
            }
        }
    }
}


static void delta16_forward(int16_t* s, size_t frames, int ch) {
    /* Convierte muestras PCM16 a diferencias para mejorar compresión */
    for (int c = 0; c < ch; ++c) {
        int16_t prev = 0;
        for (size_t i = 0; i < frames; ++i) {
            size_t idx = i*(size_t)ch + c;
            int16_t cur = s[idx];
            int16_t d = cur - prev;
            s[idx] = d;
            prev = cur;
        }
    }
}

static void delta16_inverse(int16_t* s, size_t frames, int ch) {
    /* Revierte las diferencias a muestras originales */
    for (int c = 0; c < ch; ++c) {
        int16_t prev = 0;
        for (size_t i = 0; i < frames; ++i) {
            size_t idx = i*(size_t)ch + c;
            int16_t d = s[idx];
            int16_t cur = d + prev;
            s[idx] = cur;
            prev = cur;
        }
    }
}



static void wr16le(uint8_t* p, uint16_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}

static void wr32le(uint8_t* p, uint32_t v) {
    p[0] = (v      ) & 0xFF;
    p[1] = (v >> 8 ) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

static uint16_t rd16le(const uint8_t* p) {
    return p[0] | (p[1]<<8);
}

static uint32_t rd32le(const uint8_t* p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* ---------- Formato legible de tamaño ---------- */
static void human_readable(size_t bytes, char* out, size_t out_size) {
    /* Convierte bytes a formato amigable (ej: 1.23MB) */
    const char* u[] = {"B","KB","MB","GB","TB"};
    double v = (double)bytes;
    int i = 0;
    while (v >= 1024 && i < 4) {
        v /= 1024;
        i++;
    }
    snprintf(out, out_size, "%.2f%s", v, u[i]);
}



static int compress_chunked(const Config* cfg,
                            const uint8_t* in, size_t in_len,
                            uint8_t** out, size_t* out_len)
{
    const size_t CH = cfg->chunk_bytes;
    /* Si el archivo supera un chunk se usa versión paralela; si no, bucle único. */

   
    if (in_len > CH) {
        return compress_chunked_parallel(cfg, in, in_len, out, out_len);
    }

    size_t pos = 0;

    
    size_t cap = in_len ? in_len : 1;
    *out = (uint8_t*)malloc(cap);
    if (!*out) return -1;
    *out_len = 0;

    while (pos < in_len) {
        size_t csize = in_len - pos;
        if (csize > CH) csize = CH;

        JLOG(&cfg->journal, "[JOURNAL] → Chunk %zu bytes\n", csize);

        const uint8_t* p = in + pos;
        uint8_t* bout = NULL;
        size_t blen = 0;
        int rc = 0;

        switch (cfg->comp_alg) {
            case COMP_RLEVAR:
                rc = rle_var_compress(p, csize, &bout, &blen); break;
            case COMP_LZW:
                rc = lzw_compress(p, csize, &bout, &blen); break;
            case COMP_LZWPRED: {
                uint8_t* tmp = malloc(csize);
                if (!tmp) { free(*out); return -1; }
                memcpy(tmp, p, csize);
                apply_predictor_sub(tmp, 1, 1, 1);
                rc = lzw_compress(tmp, csize, &bout, &blen);
                free(tmp);
                break;
            }
            case COMP_HUFFMANPRED: {
                uint8_t* tmp = malloc(csize);
                if (!tmp) { free(*out); return -1; }
                memcpy(tmp, p, csize);
                apply_predictor_sub(tmp, 1, 1, 1);
                rc = hp_compress_buffer(tmp, csize, &bout, &blen);
                free(tmp);
                break;
            }
            default:
                fprintf(stderr, "Algoritmo no válido.\n");
                free(*out); return -1;
        }
        if (rc != 0) { fprintf(stderr, "Error al comprimir chunk\n"); free(*out); return -1; }

        if (*out_len + blen > cap) {
            size_t ncap = cap * 2 + blen;
            uint8_t* tmp2 = (uint8_t*)realloc(*out, ncap);
            if (!tmp2) { free(bout); free(*out); return -1; }
            *out = tmp2; cap = ncap;
        }
        memcpy(*out + *out_len, bout, blen);
        *out_len += blen;
        free(bout);
        pos += csize;
    }

    uint8_t* shrink = (uint8_t*)realloc(*out, *out_len ? *out_len : 1);
    if (shrink) *out = shrink;
    return 0;
}

/* ---------- Descompresión por chunks ---------- */
static int decompress_chunked(const Config* cfg,
                              const uint8_t* in, size_t in_len,
                              uint8_t** out, size_t* out_len)
{
    const size_t CH = cfg->chunk_bytes;
    size_t pos = 0;
    /* Descomprime en bloques y reconstruye en un único buffer final */

    *out = NULL; *out_len = 0;

    while (pos < in_len) {
        size_t csize = in_len - pos;
        if (csize > CH) csize = CH;

        JLOG(&cfg->journal, "[JOURNAL] → Chunk dec (%zu bytes)\n", csize);

        const uint8_t* p = in + pos;
        uint8_t* bout = NULL;
        size_t blen = 0;
        int rc = 0;

        switch (cfg->comp_alg) {
            case COMP_RLEVAR:   rc = rle_var_decompress(p, csize, &bout, &blen); break;
            case COMP_LZW:
            case COMP_LZWPRED:  rc = lzw_decompress(p, csize, &bout, &blen); break;
            case COMP_HUFFMANPRED: rc = hp_decompress_buffer(p, csize, &bout, &blen); break;
            default: fprintf(stderr, "Algoritmo no válido.\n"); return -1;
        }
        if (rc != 0) { fprintf(stderr, "Falló descompresión chunk.\n"); free(*out); return -1; }

        if (cfg->comp_alg == COMP_LZWPRED || cfg->comp_alg == COMP_HUFFMANPRED)
            undo_predictor_sub(bout, 1, 1, 1);

        uint8_t* merged = (uint8_t*)realloc(*out, *out_len + blen ? *out_len + blen : 1);
        if (!merged) { free(bout); free(*out); return -1; }
        *out = merged;
        memcpy(*out + *out_len, bout, blen);
        *out_len += blen;
        free(bout);
        pos += csize;
    }
    return 0;
}

/* ---------- Pipeline principal: procesa un archivo completo ---------- */

static int process_one_file(const char* in, const char* out,
                            const Config* cfg,
                            size_t* o_orig, size_t* o_fin, double* o_ms)
{
    JLOG(&cfg->journal, "\n[JOURNAL] Leyendo archivo: %s\n", in);
    /* Etapas: leer -> (compresión) -> (cifrado) -> (descifrado) -> (descompresión) -> guardar */

    uint8_t* buf = NULL;
    size_t len = 0;

    if (read_file(in, &buf, &len) != 0) {
        fprintf(stderr, "Error al leer %s\n", in);
        return -1;
    }

    if (o_orig) *o_orig = len;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    uint8_t* tmp = NULL;
    size_t tlen = 0;

    int is_wav_pcm16 = 0;
    int wav_ch = 0, wav_sr = 0;
    size_t wav_frames = 0;

    if (cfg->do_c &&
        (cfg->comp_alg == COMP_DELTA16_LZW ||
         cfg->comp_alg == COMP_DELTA16_HUFF))
    {
        /* Detectar WAV y convertir a muestras para aplicar delta16 */
        if (wav_is_riff_wave(buf, len)) {
            int16_t* samples = NULL;
            if (wav_decode_pcm16(buf, len, &samples,
                                 &wav_frames, &wav_ch, &wav_sr) == 0) {

                free(buf);
                buf = (uint8_t*) samples;
                len = wav_frames * wav_ch * 2;
                is_wav_pcm16 = 1;

                JLOG(&cfg->journal, "[JOURNAL] WAV detectado (%d ch, %d SR)\n",
                       wav_ch, wav_sr);
            }
        }
    }

    /* ========== COMPRESIÓN ========== */
    if (cfg->do_c) {
        JLOG(&cfg->journal, "[JOURNAL] Iniciando compresión...\n");

        if (cfg->comp_alg == COMP_DELTA16_LZW ||
            cfg->comp_alg == COMP_DELTA16_HUFF)
        {
            if (is_wav_pcm16) {
                /* Aplicar delta y empaquetar con cabecera simple */
                delta16_forward((int16_t*)buf, wav_frames, wav_ch);

                int rc;
                if (cfg->comp_alg == COMP_DELTA16_LZW)
                    rc = lzw_compress(buf, len, &tmp, &tlen);
                else
                    rc = hp_compress_buffer(buf, len, &tmp, &tlen);

                if (rc != 0) {
                    fprintf(stderr,"Error en delta16 comp\n");
                    free(buf);
                    return -1;
                }

                size_t head = 18;
                uint8_t* pack = malloc(head + tlen);

                memcpy(pack, WAV_MAGIC, 8);
                wr16le(pack+8, wav_ch);
                wr32le(pack+10, wav_sr);
                wr32le(pack+14, wav_frames);
                memcpy(pack+head, tmp, tlen);

                free(tmp);
                free(buf);

                buf = pack;
                len = head + tlen;

                goto ENCRYPT;
            }
        }

        /* Si no es WAV-delta16 → compresión general chunked */
        if (compress_chunked(cfg, buf, len, &tmp, &tlen) != 0) {
            fprintf(stderr,"Error en compresión chunked\n");
            free(buf);
            return -1;
        }

        free(buf);
        buf = tmp;
        len = tlen;
        tmp = NULL;
        tlen = 0;

        JLOG(&cfg->journal, "[JOURNAL] Compresión lista: %zu bytes\n", len);
    }

ENCRYPT:

    /* ========== CIFRADO ========== */
    if (cfg->do_e) {
        JLOG(&cfg->journal, "[JOURNAL] Cifrando...\n");
        /* Cifrado en memoria: Vigenere XOR simple o AES */

        if (cfg->enc_alg == ENC_VIG) {
            vigenere_encrypt(buf, len, (uint8_t*)cfg->key, strlen(cfg->key));

        } else if (cfg->enc_alg == ENC_AES) {
            if (aes_encrypt_buffer(buf, len, cfg->key, &tmp, &tlen) != 0) {
                fprintf(stderr,"AES falló\n");
                free(buf);
                return -1;
            }
            free(buf);
            buf = tmp;
            len = tlen;
            tmp = NULL;
            tlen = 0;
        }
    }

    /* ========== DESCIFRADO ========== */
    if (cfg->do_u) {
        JLOG(&cfg->journal, "[JOURNAL] Descifrando...\n");
        /* Inverso del paso anterior si fue solicitado */
        if (cfg->enc_alg == ENC_VIG) {
            vigenere_decrypt(buf, len,
                             (uint8_t*)cfg->key, strlen(cfg->key));
        } else if (cfg->enc_alg == ENC_AES) {
            if (aes_decrypt_buffer(buf, len, cfg->key, &tmp, &tlen) != 0) {
                fprintf(stderr,"AES descifrado falló\n");
                free(buf);
                return -1;
            }
            free(buf);
            buf = tmp;
            len = tlen;
            tmp = NULL;
            tlen = 0;
        }
    }

    /* ========== DESCOMPRESIÓN ========== */
    if (cfg->do_d) {
        JLOG(&cfg->journal, "[JOURNAL] Descomprimiendo...\n");
        /* Si es delta16 reconstruir WAV, si no descompresión chunked normal */

        /* WAV delta16 */
        if (cfg->comp_alg == COMP_DELTA16_LZW ||
            cfg->comp_alg == COMP_DELTA16_HUFF)
        {
            if (len >= 18 && memcmp(buf, WAV_MAGIC, 8)==0) {

                uint16_t ch = rd16le(buf+8);
                uint32_t sr = rd32le(buf+10);
                uint32_t fr = rd32le(buf+14);

                const uint8_t* pay = buf+18;
                size_t pay_len = len-18;

                int rc;
                if (cfg->comp_alg==COMP_DELTA16_LZW)
                    rc = lzw_decompress(pay, pay_len, &tmp, &tlen);
                else
                    rc = hp_decompress_buffer(pay, pay_len, &tmp, &tlen);

                if (rc!=0) {
                    fprintf(stderr,"Falló descomp delta16\n");
                    free(buf);
                    return -1;
                }

                free(buf);
                buf = tmp;
                len = tlen;
                tmp = NULL;
                tlen = 0;

                delta16_inverse((int16_t*)buf, fr, ch);

                uint8_t* out_wav = NULL;
                size_t out_wav_len = 0;

                if (wav_encode_pcm16((int16_t*)buf, fr, ch, sr,
                                     &out_wav, &out_wav_len)==0)
                {
                    free(buf);
                    buf = out_wav;
                    len = out_wav_len;
                }
                goto SAVE;
            }
        }

        /* No delta16 → chunked */
        if (decompress_chunked(cfg, buf, len, &tmp, &tlen) != 0) {
            fprintf(stderr,"Error descomp chunked\n");
            free(buf);
            return -1;
        }

        free(buf);
        buf = tmp;
        len = tlen;
        tmp = NULL;
        tlen = 0;

        JLOG(&cfg->journal, "[JOURNAL] Descompresión lista (%zu bytes)\n", len);
    }

SAVE:
    JLOG(&cfg->journal, "[JOURNAL] Guardando en %s\n", out);
    /* Escribe resultado final a disco y mide tiempo total */

    int wres = write_file(out, buf, len);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec-t0.tv_sec)*1000.0 +
                (t1.tv_nsec-t0.tv_nsec)/1e6;

    if (o_fin) *o_fin = len;
    if (o_ms)  *o_ms  = ms;

    free(buf);
    return wres;
}

/* ---------- Macro de journaling ---------- */
#ifndef JPRINT
#define JPRINT(...) \
    do { if (cfg->journaling) { fprintf(stderr, __VA_ARGS__); } } while (0)
#endif

/* ---------- Análisis de argumentos ---------- */

static int parse_args(int argc, char* argv[], Config* cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->comp_alg = COMP_RLEVAR;
    cfg->enc_alg  = ENC_VIG;
    /* workers/inner_workers = 0 => auto-detectar núcleos */

    
    cfg->workers = 0;
    cfg->inner_workers = 0;
    cfg->chunk_bytes = (size_t)DEFAULT_CHUNK_MB * 1024ull * 1024ull;

    journal_init(&cfg->journal);

    static struct option long_opts[] = {
        {"comp-alg",      required_argument, 0, 1},
        {"enc-alg",       required_argument, 0, 2},
        {"journal",       no_argument,       0, 3},
        {"workers",       required_argument, 0, 4}, 
        {"inner-workers", required_argument, 0, 5}, 
        {"chunk-mb",      required_argument, 0, 6}, 
        {0,0,0,0}
    };

    int opt, idx = 0;
    while ((opt = getopt_long(argc, argv, "cdeui:o:k:j", long_opts, &idx)) != -1) {
        switch (opt) {
            case 'c': cfg->do_c = 1; break;
            case 'd': cfg->do_d = 1; break;
            case 'e': cfg->do_e = 1; break;
            case 'u': cfg->do_u = 1; break;
            case 'i': cfg->in_path  = optarg; break;
            case 'o': cfg->out_path = optarg; break;
            case 'k': cfg->key      = optarg; break;
            case 'j': journal_set_enabled(&cfg->journal, 1); break;

            case 1: 
                if      (strcmp(optarg, "rlevar") == 0)        cfg->comp_alg = COMP_RLEVAR;
                else if (strcmp(optarg, "lzw") == 0)           cfg->comp_alg = COMP_LZW;
                else if (strcmp(optarg, "lzw-pred") == 0)      cfg->comp_alg = COMP_LZWPRED;
                else if (strcmp(optarg, "huffman-pred") == 0)  cfg->comp_alg = COMP_HUFFMANPRED;
                else if (strcmp(optarg, "delta16-lzw") == 0)   cfg->comp_alg = COMP_DELTA16_LZW;
                else if (strcmp(optarg, "delta16-huff") == 0)  cfg->comp_alg = COMP_DELTA16_HUFF;
                else {
                    fprintf(stderr, "Algoritmo de compresión desconocido: %s\n", optarg);
                    return -1;
                }
                break;

            case 2: 
                if      (strcmp(optarg, "none") == 0)     cfg->enc_alg = ENC_NONE;
                else if (strcmp(optarg, "vigenere") == 0) cfg->enc_alg = ENC_VIG;
                else if (strcmp(optarg, "aes") == 0)      cfg->enc_alg = ENC_AES;
                else {
                    fprintf(stderr, "Algoritmo de cifrado desconocido: %s\n", optarg);
                    return -1;
                }
                break;

            case 3: journal_set_enabled(&cfg->journal, 1); break;

            case 4:
                if (strcmp(optarg, "auto")==0) cfg->workers = 0;
                else {
                    cfg->workers = atoi(optarg);
                    if (cfg->workers < 0) cfg->workers = 0;
                    if (cfg->workers > 64) cfg->workers = 64;
                }
                break;

            case 5: 
                if (strcmp(optarg, "auto")==0) cfg->inner_workers = 0;
                else {
                    cfg->inner_workers = atoi(optarg);
                    if (cfg->inner_workers < 0) cfg->inner_workers = 0;
                    if (cfg->inner_workers > 64) cfg->inner_workers = 64;
                }
                break;

            case 6: 
                {
                    long mb = atol(optarg);
                    if (mb < 1) mb = 1;
                    if (mb > 2048) mb = 2048; // 2GB por chunk máximo
                    cfg->chunk_bytes = (size_t)mb * 1024ull * 1024ull;
                }
                break;

            default:
                fprintf(stderr, "Opción inválida\n");
                return -1;
        }
    }

#ifdef NO_OPENSSL
    if (cfg->enc_alg == ENC_AES) {
        fprintf(stderr, "AES no disponible: instale libssl-dev y recompile.\n");
        return -1;
    }
#endif

    /* Validación mínima */
    if (!cfg->in_path || !cfg->out_path) {
        fprintf(stderr, "Debe indicar ruta de entrada -i y ruta de salida -o.\n");
        return -1;
    }

    /* Si usa AES debe haber clave */
    if (cfg->enc_alg == ENC_AES && !cfg->key) {
        fprintf(stderr, "AES requiere clave: usar -k <clave>\n");
        return -1;
    }

    return 0;
}


/* ---------- Helpers para archivos ---------- */
static int is_regular(const char* p) {
    /* Devuelve 1 si es archivo regular */
    struct stat st;
    return (stat(p, &st) == 0 && S_ISREG(st.st_mode));
}

static int is_dir(const char* p) {
    /* Devuelve 1 si es carpeta */
    struct stat st;
    return (stat(p, &st) == 0 && S_ISDIR(st.st_mode));
}

static char* join_path(const char* a, const char* b) {
    /* Une directorios asegurando slash */
    size_t na = strlen(a), nb = strlen(b);
    int need_slash = (na > 0 && a[na - 1] != '/');
    char* r = malloc(na + nb + 2);
    strcpy(r, a);
    if (need_slash) strcat(r, "/");
    strcat(r, b);
    return r;
}

/*************************************************************
 *                 LISTA DE ARCHIVOS DE CARPETA
 *************************************************************/
typedef struct {
    char** in;
    char** out;
    size_t count;
    size_t cap;
} FileList;

static void fl_init(FileList* fl) {
    /* Inicializa lista dinámica de archivos */
    fl->count = 0;
    fl->cap = 32;
    fl->in = malloc(sizeof(char*) * fl->cap);
    fl->out = malloc(sizeof(char*) * fl->cap);
}

static void fl_push(FileList* fl, char* in, char* out) {
    /* Agrega archivo (expande si hace falta) */
    if (fl->count == fl->cap) {
        fl->cap *= 2;
        fl->in = realloc(fl->in, sizeof(char*) * fl->cap);
        fl->out = realloc(fl->out, sizeof(char*) * fl->cap);
    }
    fl->in[fl->count] = in;
    fl->out[fl->count] = out;
    fl->count++;
}

static void fl_free(FileList* fl) {
    /* Libera toda la lista y rutas */
    for (size_t i = 0; i < fl->count; i++) {
        free(fl->in[i]);
        free(fl->out[i]);
    }
    free(fl->in);
    free(fl->out);
}

/*************************************************************
 *                   Estructura de Tarea
 *************************************************************/
typedef struct {
    char* in;
    char* out;
    const Config* cfg;

    int rc;
    size_t orig;
    size_t fin;
    double ms;
} Task;

static void task_run(void* arg) {
    /* Ejecuta la tarea de proceso para un archivo */
    Task* t = arg;
    t->rc = process_one_file(
        t->in,
        t->out,
        t->cfg,
        &t->orig,
        &t->fin,
        &t->ms
    );
}

/*************************************************************
 *                 MODO INTERACTIVO
 *************************************************************/
static const char* choose_comp_alg() {
    /* Pregunta por algoritmo de compresión y devuelve texto */
    printf("Algoritmo de compresión:\n");
    printf(" 1) rlevar\n");
    printf(" 2) lzw\n");
    printf(" 3) lzw-pred\n");
    printf(" 4) huffman-pred\n");
    printf(" 5) delta16-lzw\n");
    printf(" 6) delta16-huff\n> ");
    int v;
    scanf("%d", &v);
    if (v == 2) return "lzw";
    if (v == 3) return "lzw-pred";
    if (v == 4) return "huffman-pred";
    if (v == 5) return "delta16-lzw";
    if (v == 6) return "delta16-huff";
    return "rlevar";
}

static const char* choose_enc_alg() {
#ifdef NO_OPENSSL
    printf("Algoritmo de cifrado:\n");
    printf(" 1) vigenere\n");
    printf(" 2) none\n");
    printf(" (AES no disponible: compile con libssl-dev)\n> ");
    int v; scanf("%d", &v);
    return (v == 2) ? "none" : "vigenere";
#else
    printf("Algoritmo de cifrado:\n");
    printf(" 1) vigenere\n");
    printf(" 2) aes\n");
    printf(" 3) none\n> ");
    int v; scanf("%d", &v);
    if (v == 2) return "aes";
    if (v == 3) return "none";
    return "vigenere";
#endif
}

static int run_interactive() {
    /* Construye un comando equivalente usando opciones elegidas por el usuario */
    char in[512], out[512], key[512];

    printf("Ruta de entrada: ");
    scanf("%s", in);
    printf("Ruta de salida: ");
    scanf("%s", out);

    printf("Operación:\n");
    printf(" 1) Comprimir\n");
    printf(" 2) Encriptar\n");
    printf(" 3) Comprimir + Encriptar\n");
    printf(" 4) Descomprimir\n");
    printf(" 5) Desencriptar\n");
    printf(" 6) Descomprimir + Desencriptar\n> ");
    int op;
    scanf("%d", &op);

    
    char cmd[2048] = "./gsea ";

    if (op == 1 || op == 3) strcat(cmd, "-c ");
    if (op == 2 || op == 3) strcat(cmd, "-e ");
    if (op == 4 || op == 6) strcat(cmd, "-d ");
    if (op == 5 || op == 6) strcat(cmd, "-u ");

    
    if (op == 1 || op == 3 || op == 4 || op == 6) {
        const char* c = choose_comp_alg();
        strcat(cmd, "--comp-alg ");
        strcat(cmd, c);
        strcat(cmd, " ");
    }

    
    if (op == 2 || op == 3 || op == 5 || op == 6) {
        const char* e = choose_enc_alg();
        strcat(cmd, "--enc-alg ");
        strcat(cmd, e);
        strcat(cmd, " ");

        if (strcmp(e, "none") != 0) {
            printf("Clave: ");
            scanf("%s", key);
            strcat(cmd, "-k ");
            strcat(cmd, key);
            strcat(cmd, " ");
        }
    }

    
    printf("¿Activar journaling (paso a paso)? [s/N]: ");
    char jopt = 'n';
    scanf(" %c", &jopt);
    if (jopt == 's' || jopt == 'S') {
        strcat(cmd, "-j ");
    }

    strcat(cmd, "-i ");
    strcat(cmd, in);
    strcat(cmd, " -o ");
    strcat(cmd, out);

    printf("\nEjecutando: %s\n", cmd);
    return system(cmd);
}

/*************************************************************
 *                           MAIN
 *************************************************************/
int main(int argc, char* argv[])
{
    /* modo interactivo */
    if (argc == 1)
        return run_interactive();

    Config cfg;
    if (parse_args(argc, argv, &cfg) != 0)
        return 1;

    /* ¿Es archivo único o carpeta? */
    int isFolder = is_dir(cfg.in_path);

    /* Si es archivo único */
    if (!isFolder) {
        /* Modo archivo único (sin pool externo) */
        printf("Procesando archivo único...\n");

        Task t = {
            .in  = (char*)cfg.in_path,
            .out = (char*)cfg.out_path,
            .cfg = &cfg
        };

        task_run(&t);

        char oh[32], fh[32];
        human_readable(t.orig, oh, sizeof(oh));
        human_readable(t.fin, fh, sizeof(fh));

        double ahorro = (t.orig ? (1.0 - (double)t.fin / t.orig) * 100.0 : 0.0);

        printf("\nArchivo            | Orig          | Final         | Ahorro(%%) | Tiempo(ms)\n");
        printf("------------------------------------------------------------------------------\n");
        printf("%s | %zu (%s)| → %zu (%s) | %.2f%%  | %.3f ms\n",
               cfg.in_path, t.orig, oh, t.fin, fh, ahorro, t.ms);

        return 0;
    }

    /* Si es carpeta */
    printf("Procesando carpeta con hilos...\n");
    /* Modo carpeta: lista archivos regulares y usa thread pool externo */

    FileList fl;
    fl_init(&fl);

    DIR* d = opendir(cfg.in_path);
    struct dirent* de;

    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;

        char* in_full  = join_path(cfg.in_path, de->d_name);
        char* out_full = join_path(cfg.out_path, de->d_name);

        if (is_regular(in_full))
            fl_push(&fl, in_full, out_full);
        else {
            free(in_full);
            free(out_full);
        }
    }
    closedir(d);

    /* Crear carpeta de salida si no existe */
    mkdir(cfg.out_path, 0755);

    /* Pool externo con N trabajadores parametrizable */
    size_t outer = (cfg.workers > 0) ? (size_t)cfg.workers : (size_t)hw_threads();
    /* outer = hilos para archivos; inner (cfg->inner_workers) se usa dentro de compresión chunked */
    ThreadPool* tp = tp_create(outer);
    JLOG(&cfg.journal, "[JOURNAL] Pool externo: %zu hilos, inner: %d, chunk: %zu MB\n",
         outer, (cfg.inner_workers>0?cfg.inner_workers:hw_threads()), cfg.chunk_bytes/(1024*1024));

    Task* tasks = calloc(fl.count, sizeof(Task));
    for (size_t i = 0; i < fl.count; i++) {
        tasks[i].in  = fl.in[i];
        tasks[i].out = fl.out[i];
        tasks[i].cfg = &cfg;
        tp_submit(tp, task_run, &tasks[i]);
    }
    tp_wait(tp);
    tp_destroy(tp);

    /* Resultados */
    printf("\nArchivo    | Orig          | Final         | Ahorro(%%) | Tiempo(ms)\n");
    printf("----------------------------------------------------------------------\n");

    size_t total_o = 0, total_f = 0;
    double total_t = 0;

    for (size_t i = 0; i < fl.count; i++) {
        Task* t = &tasks[i];

        char oh[32], fh[32];
        human_readable(t->orig, oh, sizeof(oh));
        human_readable(t->fin, fh, sizeof(fh));

        double ahorro = (t->orig ? (1.0 - (double)t->fin / t->orig) * 100.0 : 0.0);

        printf("%s | %zu (%s)| → %zu (%s) | %.2f%%  | %.3f ms\n",
               t->in, t->orig, oh, t->fin, fh, ahorro, t->ms);

        total_o += t->orig;
        total_f += t->fin;
        total_t += t->ms;
    }

    printf("-----------------------------------------------------\n");
    printf("TOTAL: %zu → %zu  (%.2f%% ahorro)  Tiempo total: %.3f ms\n",
           total_o, total_f,
           (total_o ? (1.0 - (double)total_f / total_o) * 100.0 : 0.0),
           total_t);

    fl_free(&fl);
    free(tasks);

    return 0;
}

/* ---------- Helpers para cantidad de hilos ---------- */
static int hw_threads(void) {
    /* Obtiene núcleos lógicos disponibles (con límites) */
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0) return 4;
    if (n > 128) n = 128;
    return (int)n;
}

/* ========== Paralelismo interno por chunks ========== */
typedef struct {
    const Config* cfg;
    const uint8_t* in;
    size_t len;
    size_t chunk_id;
    uint8_t* out;
    size_t out_len;
    int err;
} ChunkTask;

static void compress_chunk_worker(void* arg) {
    /* Comprime 1 chunk (posible predictor) y guarda resultado */
    ChunkTask* ct = (ChunkTask*)arg;
    const uint8_t* p = ct->in; size_t n = ct->len;
    uint8_t* bout = NULL; size_t blen = 0; int rc = 0;

    switch (ct->cfg->comp_alg) {
        case COMP_RLEVAR:   rc = rle_var_compress(p, n, &bout, &blen); break;
        case COMP_LZW:      rc = lzw_compress(p, n, &bout, &blen); break;
        case COMP_LZWPRED: {
            uint8_t* tmp = malloc(n); if (!tmp){ ct->err=-1; return; }
            memcpy(tmp, p, n); apply_predictor_sub(tmp, 1, 1, 1);
            rc = lzw_compress(tmp, n, &bout, &blen); free(tmp); break;
        }
        case COMP_HUFFMANPRED: {
            uint8_t* tmp = malloc(n); if (!tmp){ ct->err=-1; return; }
            memcpy(tmp, p, n); apply_predictor_sub(tmp, 1, 1, 1);
            rc = hp_compress_buffer(tmp, n, &bout, &blen); free(tmp); break;
        }
        default: rc = -1;
    }
    ct->err = rc; ct->out = bout; ct->out_len = blen;
}

static int compress_chunked_parallel(const Config* cfg,
                                     const uint8_t* in, size_t in_len,
                                     uint8_t** out, size_t* out_len)
{
    /* Divide archivo en chunks y los procesa con hilos internos, luego concatena */
    const size_t CH = cfg->chunk_bytes;
    size_t n_chunks = (in_len + CH - 1) / CH;

    int wanted = (cfg->inner_workers > 1) ? cfg->inner_workers : hw_threads();
    if (wanted > (int)n_chunks) wanted = (int)n_chunks;
    if (wanted < 1) wanted = 1;

    JLOG(&cfg->journal, "[JOURNAL] Paralelo: %zu chunks con %d hilos\n", n_chunks, wanted);

    ThreadPool* tp = tp_create((size_t)wanted);
    if (!tp) return -1;

    ChunkTask* tasks = (ChunkTask*)calloc(n_chunks, sizeof(ChunkTask));
    if (!tasks) { tp_destroy(tp); return -1; }

    for (size_t i = 0; i < n_chunks; i++) {
        size_t off = i * CH;
        size_t sz  = (off + CH > in_len) ? (in_len - off) : CH;
        tasks[i].cfg = cfg;
        tasks[i].in  = in + off;
        tasks[i].len = sz;
        tasks[i].chunk_id = i;
        tp_submit(tp, compress_chunk_worker, &tasks[i]);
    }

    tp_wait(tp);
    tp_destroy(tp);

    size_t total = 0;
    for (size_t i = 0; i < n_chunks; i++) {
        if (tasks[i].err) {
            for (size_t j = 0; j < n_chunks; j++) free(tasks[j].out);
            free(tasks); return -1;
        }
        total += tasks[i].out_len;
    }

    uint8_t* buf = (uint8_t*)malloc(total ? total : 1);
    if (!buf) { for (size_t j = 0; j < n_chunks; j++) free(tasks[j].out); free(tasks); return -1; }

    size_t k = 0;
    for (size_t i = 0; i < n_chunks; i++) {
        memcpy(buf + k, tasks[i].out, tasks[i].out_len);
        k += tasks[i].out_len;
        free(tasks[i].out);
    }
    free(tasks);

    *out = buf; *out_len = total;
    return 0;
}
