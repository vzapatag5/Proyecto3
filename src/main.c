#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>     // getopt / getopt_long, getcwd
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <dirent.h>
#include <pthread.h>

#include "fs.h"
#include "rle_var.h"
#include "vigenere.h"
#include "image_png.h"
#include "image_jpeg.h"
#include "lzw.h"

/* Contenedor / formatos usados por el pipeline */
#define MAGIC_HDR     "GSEAIMG1"
#define MAGIC_HDR_LEN 8
#define FMT_PNG  1
#define FMT_JPEG 2

/* Evita warnings en algunos entornos */
extern int optind;
extern char *optarg;

/* Algoritmos soportados */
typedef enum { COMP_RLEVAR, COMP_LZW, COMP_LZWPRED } CompAlg;
typedef enum { ENC_NONE, ENC_VIG } EncAlg;

/* Config CLI */
typedef struct {
    int do_c, do_d, do_e, do_u;     // operaciones
    const char* in_path;            // -i
    const char* out_path;           // -o (se forzará a tests/<...>)
    const char* key;                // -k (obligatoria con -e o -u)
    CompAlg comp_alg;               // --comp-alg
    EncAlg  enc_alg;                // --enc-alg
} Config;

/* ---------- Ayuda ---------- */
static void print_help(void) {
    puts("Uso:");
    puts("  ./gsea [-c|-d] [-e|-u] --comp-alg rlevar --enc-alg [vigenere|none] -i <in> -o <out> [-k <clave>]");
    puts("");
    puts("Operaciones (combinables):");
    puts("  -c  Comprimir");
    puts("  -d  Descomprimir");
    puts("  -e  Encriptar");
    puts("  -u  Desencriptar");
    puts("");
    puts("Algoritmos:");
    puts("  --comp-alg rlevar");
    puts("  --enc-alg  vigenere | none");
    puts("");
    puts("Ejemplos:");
    puts("  # Comprimir + Encriptar (archivo)");
    puts("  ./gsea -c -e --comp-alg rlevar --enc-alg vigenere -i tests/a.txt -o out.bin -k 1234");
    puts("");
    puts("  # Desencriptar + Descomprimir (archivo)");
    puts("  ./gsea -u -d --comp-alg rlevar --enc-alg vigenere -i tests/out.bin -o back.txt -k 1234");
    puts("");
    puts("  # Comprimir + Encriptar (directorio recursivo, concurrente: un hilo por archivo)");
    puts("  ./gsea -c -e -i data/ -o outdir/ -k 1234");
}

/* ---------- Utilidades FS y rutas ---------- */

/* Forzar salida en tests/ */
static int  ensure_tests_dir(void);
static char* build_tests_output_path(const char* out_path);
static char* join2(const char* a, const char* b); /* <-- prototipo añadido aquí */
static int run_interactive(void); /* forward prototype for interactive menu */

/* Quita slashes finales de una ruta y devuelve una copia (malloc). */
static char* trim_trailing_slashes(const char* p) {
    if (!p) return NULL;
    size_t n = strlen(p);
    if (n == 0) return strdup(p);
    char* s = (char*)malloc(n + 1);
    if (!s) return NULL;
    memcpy(s, p, n + 1);
    /* Mantener al menos 1 char (no vaciar "/" raíz) */
    while (n > 1 && s[n-1] == '/') { s[--n] = '\0'; }
    return s;
}

// Asegura que el directorio tests/ exista
static int ensure_tests_dir(void) {
    struct stat st;
    if (stat("tests", &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        fprintf(stderr, "Existe un archivo llamado 'tests' que no es directorio\n");
        return -1;
    }
    if (mkdir("tests", 0755) == 0) return 0;
    if (errno == EEXIST) return 0;
    perror("mkdir tests");
    return -1;
}

/* tests/<basename(out_path)> */
// Si out_path ya empieza con tests/, devolver tal cual
static char* build_tests_output_path(const char* out_path) {
    /* Recorta slashes finales para que -o "outdir/" => basename "outdir" */
    char* out_trim = trim_trailing_slashes(out_path);
    if (!out_trim) return NULL;

    /* Si ya es tests/... devolvemos tal cual (evita "tests/tests/...") */
    if (strncmp(out_trim, "tests/", 6) == 0 || strcmp(out_trim, "tests") == 0) {
        char* dup = strdup(out_trim);
        free(out_trim);
        return dup;
    }

    /* Si es ruta absoluta, mantenemos el comportamiento antiguo (solo basename)
       para evitar crear fuera de tests/. */
    if (out_trim[0] == '/') {
        const char* base = strrchr(out_trim, '/');
    #ifdef _WIN32
        const char* b2 = strrchr(out_trim, '\\');
        if (!base || (b2 && b2 > base)) base = b2;
    #endif
        base = base ? base + 1 : out_trim;
        if (*base == '\0') base = "out"; /* fallback por si queda vacío */

        size_t need = strlen("tests/") + strlen(base) + 1;
        char* dst = (char*)malloc(need);
        if (!dst) { free(out_trim); return NULL; }
        strcpy(dst, "tests/");
        strcat(dst, base);
        free(out_trim);
        return dst;
    }

    /* Ruta relativa (no empieza con tests/): preservamos subdirectorios bajo tests/ */
    char* dst = join2("tests", out_trim);
    free(out_trim);
    return dst;
}

// Comprueba si path es un directorio
static int is_dir(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

// Comprueba si path es un archivo regular
static int is_regular(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode);
}

/* Une rutas con '/' (malloc) */
// Ejemplo: join2("a/b", "c/d") => "a/b/c/d"
static char* join2(const char* a, const char* b) {
    if (!a || !b) return NULL;
    size_t na = strlen(a), nb = strlen(b);
    int need_slash = (na > 0 && a[na-1] != '/');
    size_t need = na + (need_slash ? 1 : 0) + nb + 1;
    char* s = (char*)malloc(need);
    if (!s) return NULL;
    strcpy(s, a);
    if (need_slash) strcat(s, "/");
    strcat(s, b);
    return s;
}

/* mkdir -p */
static int mkdirs(const char* path, mode_t mode) {
    if (!path || !*path) return -1;

    /* Si la ruta comienza por "data" (p.ej. "data" o "data/...") y no está ya
       dentro de tests/, reescribimos a "tests/<ruta>" */
    const char* use_path = path;
    char* prepended = NULL;
    if ((strncmp(path, "data", 4) == 0) && (path[4] == '/' || path[4] == '\0')) {
        if (strncmp(path, "tests/", 6) != 0) {
            prepended = join2("tests", path);
            if (!prepended) return -1;
            use_path = prepended;
        }
    }

    char* tmp = strdup(use_path);
    if (!tmp) { free(prepended); return -1; }

    size_t len = strlen(tmp);
    if (len == 0) { free(tmp); free(prepended); return -1; }
    if (tmp[len-1] == '/') tmp[len-1] = '\0';

    for (char* p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) { free(tmp); free(prepended); return -1; }
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) { free(tmp); free(prepended); return -1; }
    free(tmp);
    free(prepended);
    return 0;
}

/* Ruta absoluta simple */
// Si p es relativa, la convierte a absoluta usando getcwd()
static char* to_abs(const char* p) {
    if (!p) return NULL;
    if (p[0] == '/') return strdup(p);
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) return NULL;
    return join2(cwd, p);
}

/* tests/<basename(out_root)>/<relpath>, relpath = in_abs - prefijo in_root_abs */
// Ejemplo: in_abs="/home/user/data/a/b.txt", in_root_abs="/home/user/data", out_root="outdir"
static char* out_path_for(const char* in_abs, const char* in_root_abs, const char* out_root) {
    if (ensure_tests_dir() != 0) return NULL;

    /* Trim para que "-o outdir/" => basename "outdir" */
    char* out_root_trim = trim_trailing_slashes(out_root);
    if (!out_root_trim) return NULL;

    const char* base = strrchr(out_root_trim, '/');
#ifdef _WIN32
    const char* b2 = strrchr(out_root_trim, '\\');
    if (!base || (b2 && b2 > base)) base = b2;
#endif
    base = base ? base + 1 : out_root_trim;
    if (*base == '\0') base = "out"; /* fallback */

    size_t nr = strlen(in_root_abs);
    const char* rel = in_abs;
    if (strncmp(in_abs, in_root_abs, nr) == 0) {
        rel = in_abs + nr;
        if (*rel == '/') rel++;
    }

    char* t1 = join2("tests", base);  if (!t1) { free(out_root_trim); return NULL; }
    char* outp = join2(t1, rel);      if (!outp) { free(t1); free(out_root_trim); return NULL; }
    free(t1);
    free(out_root_trim);
    return outp; /* caller free */
}

/* ---------- Parser de argumentos ---------- */
static int parse_args(int argc, char* argv[], Config* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->comp_alg = COMP_RLEVAR;      // default
    cfg->enc_alg  = ENC_VIG;          // default

    static struct option long_opts[] = {
        {"comp-alg", required_argument, 0, 1},
        {"enc-alg",  required_argument, 0, 2},
        {0, 0, 0, 0}
    };

    int opt, long_index = 0;
    optind = 1;

    while ((opt = getopt_long(argc, argv, "cdeui:o:k:", long_opts, &long_index)) != -1) {
        switch (opt) {
            case 'c': cfg->do_c = 1; break;
            case 'd': cfg->do_d = 1; break;
            case 'e': cfg->do_e = 1; break;
            case 'u': cfg->do_u = 1; break;
            case 'i': cfg->in_path = optarg; break;
            case 'o': cfg->out_path = optarg; break;
            case 'k': cfg->key = optarg; break;

            case 1: /* --comp-alg */
                if (strcmp(optarg, "rlevar") == 0) {
                    cfg->comp_alg = COMP_RLEVAR;
                } else if (strcmp(optarg, "lzw") == 0) {
                    cfg->comp_alg = COMP_LZW;
                } else if (strcmp(optarg, "lzw-pred") == 0) {
                    cfg->comp_alg = COMP_LZWPRED;
                } else {
                    fprintf(stderr, "Algoritmo de compresión desconocido: %s\n", optarg);
                    return -1;
                }
                break;

            case 2: /* --enc-alg */
                if      (strcmp(optarg, "vigenere") == 0) cfg->enc_alg = ENC_VIG;
                else if (strcmp(optarg, "none")     == 0) cfg->enc_alg = ENC_NONE;
                else {
                    fprintf(stderr, "Algoritmo de cifrado desconocido: %s\n", optarg);
                    return -1;
                }
                break;

            default:
                return -1;
        }
    }

    int ops = (cfg->do_c?1:0) + (cfg->do_d?1:0) + (cfg->do_e?1:0) + (cfg->do_u?1:0);
    if (ops == 0) {
        fprintf(stderr, "Debe especificar al menos una operación (-c/-d/-e/-u)\n");
        return -1;
    }
    if (cfg->do_c && cfg->do_d) { fprintf(stderr, "No puede usar -c y -d en la misma invocación\n"); return -1; }
    if (cfg->do_e && cfg->do_u) { fprintf(stderr, "No puede usar -e y -u en la misma invocación\n"); return -1; }
    if ((cfg->do_e || cfg->do_u) && (!cfg->key || cfg->key[0] == '\0')) {
        fprintf(stderr, "Debe proporcionar clave (-k) si usa -e o -u\n");
        return -1;
    }
    if (!cfg->in_path || !cfg->out_path) {
        fprintf(stderr, "Debe indicar entrada (-i) y salida (-o)\n");
        return -1;
    }
    return 0;
}

/* ---------- Pipeline para UN archivo (reemplaza la versión previa) ---------- */
/* Predictor (Sub) used before LZW to decorrelate pixels horizontally.
 * in-place transform: for each row, for each channel, out = cur - left (left is previous original channel value), left updated to original
 */
static void apply_predictor_sub(uint8_t* buf, int w, int h, int ch) {
    if (!buf || w <= 0 || h <= 0 || ch <= 0) return;
    for (int y = 0; y < h; ++y) {
        size_t row_off = (size_t)y * w * ch;
        /* left values per channel */
        uint8_t left[4] = {0,0,0,0};
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < ch; ++c) {
                size_t idx = row_off + (size_t)x * ch + c;
                uint8_t cur = buf[idx];
                uint8_t pred = (uint8_t)(cur - left[c]);
                buf[idx] = pred;
                left[c] = cur;
            }
        }
    }
}

static void undo_predictor_sub(uint8_t* buf, int w, int h, int ch) {
    if (!buf || w <= 0 || h <= 0 || ch <= 0) return;
    for (int y = 0; y < h; ++y) {
        size_t row_off = (size_t)y * w * ch;
        uint8_t left[4] = {0,0,0,0};
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < ch; ++c) {
                size_t idx = row_off + (size_t)x * ch + c;
                uint8_t pred = buf[idx];
                uint8_t orig = (uint8_t)(pred + left[c]);
                buf[idx] = orig;
                left[c] = orig;
            }
        }
    }
}

static int process_one_file(const char* in_path, const char* out_path, const Config* cfg) {
    uint8_t* buf = NULL; size_t len = 0;
    if (read_file(in_path, &buf, &len) != 0) {
        fprintf(stderr, "read_file fallo: %s\n", in_path);
        return -1;
    }

    uint8_t* tmp = NULL; size_t tlen = 0;

    int img_w=0,img_h=0,img_ch=0;
    int was_image_input = 0;     /* entrada original era imagen (png/jpeg) */
    int img_fmt = 0;            /* FMT_PNG / FMT_JPEG if image input */
    int was_container = 0;      /* entrada era nuestro contenedor */
    int will_write_container = 0;
    int will_reconstruct_image = 0;

    /* 1) Detectar contenedor propio (plaintext header)
       Header: MAGIC(8) | fmt(1) | channels(1) | width(4 BE) | height(4 BE) */
    if (len >= (size_t)(MAGIC_HDR_LEN + 1 + 1 + 4 + 4) && memcmp(buf, MAGIC_HDR, MAGIC_HDR_LEN) == 0) {
        was_container = 1;
        uint8_t fmt = (uint8_t)buf[MAGIC_HDR_LEN];
        uint8_t ch  = (uint8_t)buf[MAGIC_HDR_LEN+1];
        uint32_t w = (uint32_t)((buf[MAGIC_HDR_LEN+2]<<24) | (buf[MAGIC_HDR_LEN+3]<<16) | (buf[MAGIC_HDR_LEN+4]<<8) | (buf[MAGIC_HDR_LEN+5]));
        uint32_t h = (uint32_t)((buf[MAGIC_HDR_LEN+6]<<24) | (buf[MAGIC_HDR_LEN+7]<<16) | (buf[MAGIC_HDR_LEN+8]<<8) | (buf[MAGIC_HDR_LEN+9]));
        img_fmt = (int)fmt;
        img_ch = (int)ch;
        img_w = (int)w; img_h = (int)h;
        size_t payload_off = MAGIC_HDR_LEN + 1 + 1 + 4 + 4;
        size_t payload_len = len - payload_off;
        uint8_t* payload = (uint8_t*)malloc(payload_len ? payload_len : 1);
        if (!payload) { free(buf); return -1; }
        memcpy(payload, buf + payload_off, payload_len);
        free(buf); buf = payload; len = payload_len;
        will_reconstruct_image = 1;
    } else {
        /* 2) Detectar por firma si es PNG o JPEG y sólo entonces llamar al decoder
           Esto evita que las libs impriman errores cuando intentan decodificar bytes no correspondientes. */
        uint8_t* pixels = NULL; size_t pixels_len = 0;
        int w=0,h=0,ch=0;

        /* PNG signature: 89 50 4E 47 0D 0A 1A 0A */
        const unsigned char png_sig[8] = {137,80,78,71,13,10,26,10};
        if (len >= 8 && memcmp(buf, png_sig, 8) == 0) {
            if (png_decode_image(buf, len, &pixels, &pixels_len, &w, &h, &ch) == 0) {
                free(buf);
                buf = pixels; len = pixels_len;
                img_w = w; img_h = h; img_ch = ch;
                was_image_input = 1; img_fmt = FMT_PNG;
                if (cfg->do_c || cfg->do_e) will_write_container = 1;
            } else {
                fprintf(stderr, "Advertencia: PNG detectado pero no pudo decodificarse (archivo corrupto?). Tratando como bytes crudos.\n");
            }
        } else if (len >= 3 && buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF) {
            if (jpeg_decode_image(buf, len, &pixels, &pixels_len, &w, &h, &ch) == 0) {
                free(buf);
                buf = pixels; len = pixels_len;
                img_w = w; img_h = h; img_ch = ch;
                was_image_input = 1; img_fmt = FMT_JPEG;
                if (cfg->do_c || cfg->do_e) will_write_container = 1;
            } else {
                fprintf(stderr, "Advertencia: JPEG detectado pero no pudo decodificarse (archivo corrupto?). Tratando como bytes crudos.\n");
            }
        } else {
            /* No es PNG ni JPEG por firma -> tratamos como fichero binario */
        }
    }

    /* FORWARD */
    if (cfg->do_c) {
        if (cfg->comp_alg == COMP_RLEVAR) {
            if (rle_var_compress(buf, len, &tmp, &tlen) != 0) { puts("RLE-Var: compresión falló"); free(buf); return -1; }
            free(buf); buf = tmp; len = tlen;
        } else if (cfg->comp_alg == COMP_LZW) {
            if (lzw_compress(buf, len, &tmp, &tlen) != 0) { fprintf(stderr, "LZW: compresión falló\n"); free(buf); return -1; }
            free(buf); buf = tmp; len = tlen;
        } else if (cfg->comp_alg == COMP_LZWPRED) {
            /* predictor only makes sense for decoded images */
            if (was_image_input && img_w > 0 && img_h > 0 && img_ch > 0) {
                apply_predictor_sub(buf, img_w, img_h, img_ch);
                if (lzw_compress(buf, len, &tmp, &tlen) != 0) { fprintf(stderr, "LZW-PRED: compresión falló\n"); free(buf); return -1; }
                free(buf); buf = tmp; len = tlen;
            } else {
                /* fallback to plain LZW on raw data */
                if (lzw_compress(buf, len, &tmp, &tlen) != 0) { fprintf(stderr, "LZW: compresión falló\n"); free(buf); return -1; }
                free(buf); buf = tmp; len = tlen;
            }
        }
    }
    if (cfg->do_e) {
        if (cfg->enc_alg == ENC_VIG) {
            vigenere_encrypt(buf, len, (const uint8_t*)cfg->key, strlen(cfg->key));
        }
    }

    /* BACKWARD */
    if (cfg->do_u) {
        if (cfg->enc_alg == ENC_VIG) {
            vigenere_decrypt(buf, len, (const uint8_t*)cfg->key, strlen(cfg->key));
        }
    }
    if (cfg->do_d) {
        if (cfg->comp_alg == COMP_RLEVAR) {
            if (rle_var_decompress(buf, len, &tmp, &tlen) != 0) { puts("RLE-Var: descompresión falló"); free(buf); return -1; }
            free(buf); buf = tmp; len = tlen;
        } else if (cfg->comp_alg == COMP_LZW || cfg->comp_alg == COMP_LZWPRED) {
            if (lzw_decompress(buf, len, &tmp, &tlen) != 0) { fprintf(stderr, "LZW: descompresión falló\n"); free(buf); return -1; }
            free(buf); buf = tmp; len = tlen;
            /* if predictor was used, undo it now (only if we have image dimensions) */
            if (cfg->comp_alg == COMP_LZWPRED && img_w > 0 && img_h > 0 && img_ch > 0) {
                undo_predictor_sub(buf, img_w, img_h, img_ch);
            }
        }
    }

    /* Si la entrada era contenedor propio y se hicieron las operaciones inversas,
       reconstruir la imagen (encode según fmt almacenado) */
    if (will_reconstruct_image) {
        uint8_t* out_img = NULL; size_t out_img_len = 0;
        int rc = -1;
        if (img_fmt == FMT_PNG) {
            rc = png_encode_image(buf, len, img_w, img_h, img_ch, &out_img, &out_img_len);
        } else if (img_fmt == FMT_JPEG) {
            /* jpeg_encode_image acepta sólo RGB (channels==3) */
            if (img_ch == 4) {
                /* strip alpha before JPEG encoding */
                size_t pixels = (size_t)img_w * img_h;
                uint8_t* rgb = (uint8_t*)malloc(pixels * 3);
                if (rgb) {
                    for (size_t i = 0; i < pixels; ++i) {
                        rgb[i*3+0] = buf[i*4+0];
                        rgb[i*3+1] = buf[i*4+1];
                        rgb[i*3+2] = buf[i*4+2];
                    }
                    rc = jpeg_encode_image(rgb, pixels*3, img_w, img_h, 3, &out_img, &out_img_len);
                    free(rgb);
                }
            } else {
                rc = jpeg_encode_image(buf, len, img_w, img_h, img_ch, &out_img, &out_img_len);
            }
        }
        if (rc == 0) {
            free(buf);
            buf = out_img; len = out_img_len;
        } else {
            /* si no se pudo re-encodear, dejamos buf tal cual */
        }
    }

    /* Preparar buffer final para escribir: si debemos escribir contenedor (imagen -> contenedor),
       construimos header (plaintext) + payload */
    uint8_t* final_buf = NULL; size_t final_len = 0;
    if (will_write_container) {
        size_t hdr_len = MAGIC_HDR_LEN + 1 + 1 + 4 + 4;
        final_len = hdr_len + len;
        final_buf = (uint8_t*)malloc(final_len ? final_len : 1);
        if (!final_buf) { free(buf); return -1; }
        memcpy(final_buf, MAGIC_HDR, MAGIC_HDR_LEN);
        final_buf[MAGIC_HDR_LEN] = (uint8_t)img_fmt;
        final_buf[MAGIC_HDR_LEN+1] = (uint8_t)img_ch;
        final_buf[MAGIC_HDR_LEN+2] = (uint8_t)((img_w >> 24) & 0xFF);
        final_buf[MAGIC_HDR_LEN+3] = (uint8_t)((img_w >> 16) & 0xFF);
        final_buf[MAGIC_HDR_LEN+4] = (uint8_t)((img_w >> 8) & 0xFF);
        final_buf[MAGIC_HDR_LEN+5] = (uint8_t)(img_w & 0xFF);
        final_buf[MAGIC_HDR_LEN+6] = (uint8_t)((img_h >> 24) & 0xFF);
        final_buf[MAGIC_HDR_LEN+7] = (uint8_t)((img_h >> 16) & 0xFF);
        final_buf[MAGIC_HDR_LEN+8] = (uint8_t)((img_h >> 8) & 0xFF);
        final_buf[MAGIC_HDR_LEN+9] = (uint8_t)(img_h & 0xFF);
        memcpy(final_buf + hdr_len, buf, len);
    } else {
        final_buf = buf;
        final_len = len;
        buf = NULL; /* transfer ownership */
    }

    /* Asegurar directorio destino y escribir */
    char* out_dir = strdup(out_path);
    if (!out_dir) { free(final_buf); free(buf); return -1; }
    char* slash = strrchr(out_dir, '/');
    if (slash) {
        *slash = '\0';
        if (mkdirs(out_dir, 0755) != 0) { perror("mkdirs"); free(out_dir); free(final_buf); free(buf); return -1; }
    }
    free(out_dir);

    if (write_file(out_path, final_buf, final_len) != 0) {
        fprintf(stderr, "write_file fallo: %s\n", out_path);
        free(final_buf); free(buf);
        return -1;
    }

    free(final_buf);
    free(buf);
    return 0;
}

/* ---------- Concurrencia (un hilo por archivo) ---------- */
typedef struct {
    char* in_path;
    char* out_path;
    const Config* cfg;  // solo lectura
    int result;         // 0 ok, !=0 error
} Task;

static void* thread_routine(void* arg) {
    Task* t = (Task*)arg;
    t->result = process_one_file(t->in_path, t->out_path, t->cfg);
    return NULL;
}

/* ---------- Recolección de archivos (recursiva) ---------- */
typedef struct {
    char** in_paths;
    char** out_paths;
    size_t count;
    size_t cap;
} PathList;

static void pl_init(PathList* pl) { pl->in_paths=NULL; pl->out_paths=NULL; pl->count=0; pl->cap=0; }
static void pl_free(PathList* pl) {
    for (size_t i=0;i<pl->count;i++){ free(pl->in_paths[i]); free(pl->out_paths[i]); }
    free(pl->in_paths); free(pl->out_paths);
}
static int pl_push(PathList* pl, char* in_p, char* out_p) {
    if (pl->count == pl->cap) {
        size_t ncap = pl->cap ? pl->cap*2 : 64;
        char** ni = (char**)realloc(pl->in_paths,  ncap*sizeof(char*));
        char** no = (char**)realloc(pl->out_paths, ncap*sizeof(char*));
        if (!ni || !no) { free(ni); free(no); return -1; }
        pl->in_paths = ni; pl->out_paths = no; pl->cap = ncap;
    }
    pl->in_paths[pl->count]  = in_p;
    pl->out_paths[pl->count] = out_p;
    pl->count++;
    return 0;
}

static char* out_path_for(const char* in_abs, const char* in_root_abs, const char* out_root); /* fwd */

static int collect_files_recursive(const char* in_root_abs, const char* cur_abs, const char* out_root_abs, PathList* pl) {
    DIR* d = opendir(cur_abs);
    if (!d) { perror("opendir"); return -1; }

    struct dirent* de;
    int status = 0;

    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name,".") || !strcmp(de->d_name,"..")) continue;

        char* child = join2(cur_abs, de->d_name);
        if (!child) { status = -1; break; }

        struct stat st;
        if (stat(child, &st) != 0) { perror("stat"); free(child); status = -1; break; }

        if (S_ISDIR(st.st_mode)) {
            if (collect_files_recursive(in_root_abs, child, out_root_abs, pl) != 0) { free(child); status = -1; break; }
            free(child);
        } else if (S_ISREG(st.st_mode)) {
            char* outp = out_path_for(child, in_root_abs, out_root_abs);
            if (!outp) { free(child); status = -1; break; }
            if (pl_push(pl, child, outp) != 0) { free(child); free(outp); status = -1; break; }
        } else {
            free(child);
        }
    }
    closedir(d);
    return status;
}

/* ---------- main ---------- */
int main(int argc, char* argv[]) {
    /* Si no se pasan argumentos, lanzar menú interactivo */
    if (argc == 1) {
        return run_interactive();
    }

    Config cfg;
    if (parse_args(argc, argv, &cfg) != 0) { print_help(); return 1; }

    /* Normalizar rutas a "absolutas simples" */
    char* in_abs  = to_abs(cfg.in_path);
    char* out_abs = to_abs(cfg.out_path);
    if (!in_abs || !out_abs) {
        free(in_abs); free(out_abs);
        fprintf(stderr, "Rutas inválidas\n");
        return 1;
    }

    if (!is_dir(in_abs)) {
        /* ARCHIVO ÚNICO: salida en tests/<basename(-o)> */
        if (!is_regular(in_abs)) {
            fprintf(stderr, "La entrada no es archivo regular ni directorio: %s\n", in_abs);
            free(in_abs); free(out_abs);
            return 1;
        }
        if (ensure_tests_dir() != 0) { free(in_abs); free(out_abs); return 1; }
        char* tests_out = build_tests_output_path(cfg.out_path);
        if (!tests_out) {
            fprintf(stderr, "No se pudo construir ruta de salida\n");
            free(in_abs); free(out_abs); return 1;
        }

        if (process_one_file(in_abs, tests_out, &cfg) != 0) {
            free(tests_out); free(in_abs); free(out_abs); return 1;
        }
        printf("Listo ✅ -> %s\n", tests_out);
        free(tests_out);
    } else {
        /* DIRECTORIO: recolectar y lanzar UN HILO POR ARCHIVO */
        PathList pl; pl_init(&pl);

        if (collect_files_recursive(in_abs, in_abs, out_abs, &pl) != 0) {
            pl_free(&pl); free(in_abs); free(out_abs); return 1;
        }
        if (pl.count == 0) {
            printf("Directorio vacío: %s\n", in_abs);
            pl_free(&pl); free(in_abs); free(out_abs); return 0;
        }

        if (ensure_tests_dir() != 0) { pl_free(&pl); free(in_abs); free(out_abs); return 1; }

        pthread_t* tids = (pthread_t*)malloc(pl.count * sizeof(pthread_t));
        Task* tasks = (Task*)calloc(pl.count, sizeof(Task));
        if (!tids || !tasks) { free(tids); free(tasks); pl_free(&pl); free(in_abs); free(out_abs); return 1; }

        for (size_t i=0; i<pl.count; ++i) {
            tasks[i].in_path  = pl.in_paths[i];
            tasks[i].out_path = pl.out_paths[i];
            tasks[i].cfg      = &cfg;
            tasks[i].result   = -1;

            int rc = pthread_create(&tids[i], NULL, thread_routine, &tasks[i]);
            if (rc != 0) {
                fprintf(stderr, "pthread_create fallo para %s\n", pl.in_paths[i]);
                tasks[i].result = -1;
            }
        }

        int global_status = 0;
        for (size_t i=0; i<pl.count; ++i) {
            pthread_join(tids[i], NULL);
            if (tasks[i].result != 0) {
                fprintf(stderr, "ERROR en: %s -> %s\n", tasks[i].in_path, tasks[i].out_path);
                global_status = 1;
            } else {
                printf("OK: %s -> %s\n", tasks[i].in_path, tasks[i].out_path);
            }
        }

        free(tids);
        free(tasks);
        pl_free(&pl);

        printf(global_status ? "Terminado con errores ❌ (directorio)\n"
                             : "Listo ✅ (directorio, concurrente)\n");

        free(in_abs);
        free(out_abs);
        return global_status;
    }

    free(in_abs);
    free(out_abs);
    return 0;
}
/* --------- Menú interactivo (invocado si argc==1) --------- */
static char* prompt_line(const char* prompt) {
    if (!prompt) prompt = "> ";
    printf("%s", prompt);
    fflush(stdout);
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) return NULL;
    size_t n = strlen(buf);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) { buf[--n] = '\0'; }
    if (n == 0) return NULL;
    return strdup(buf);
}

static int prompt_yesno(const char* prompt) {
    char* r = prompt_line(prompt);
    if (!r) return 0;
    int ok = (r[0]=='y' || r[0]=='Y' || r[0]=='1');
    free(r);
    return ok;
}

static const char* choose_comp_alg_str(void) {
    printf("Elige algoritmo de compresión:\n");
    printf("  1) rlevar (default)\n");
    printf("  2) lzw\n");
    printf("  3) lzw-pred\n");
    printf("Selecciona (1-3): ");
    char buf[16];
    if (!fgets(buf, sizeof(buf), stdin)) return "rlevar";
    int v = atoi(buf);
    if (v == 2) return "lzw";
    if (v == 3) return "lzw-pred";
    return "rlevar";
}

static const char* choose_enc_alg_str(void) {
    printf("Elige algoritmo de cifrado:\n");
    printf("  1) vigenere (default)\n");
    printf("  2) none\n");
    printf("Selecciona (1-2): ");
    char buf[16];
    if (!fgets(buf, sizeof(buf), stdin)) return "vigenere";
    int v = atoi(buf);
    if (v == 2) return "none";
    return "vigenere";
}

static int run_interactive(void) {
    puts("=== GSEA - modo interactivo ===");
    char* in = prompt_line("Ruta de entrada (-i): ");
    if (!in) { puts("Entrada inválida."); return 1; }
    char* out = prompt_line("Ruta de salida (-o): ");
    if (!out) { free(in); puts("Salida inválida."); return 1; }

    int do_c = prompt_yesno("¿Comprimir? (y/N): ");
    int do_d = 0;
    int do_e = prompt_yesno("¿Encriptar? (y/N): ");
    int do_u = 0;
    if (!do_c && !do_e) {
        puts("Ninguna operación seleccionada. Puedes combinar compresión (-c) y/o encriptado (-e).");
    }

    const char* comp_alg = choose_comp_alg_str();
    const char* enc_alg = choose_enc_alg_str();

    char* key = NULL;
    if (strcmp(enc_alg, "vigenere") == 0 && do_e) {
        key = prompt_line("Clave (para Vigenere) (-k): ");
        if (!key) { puts("Clave vacía, abortando."); free(in); free(out); return 1; }
    } else if (strcmp(enc_alg, "vigenere") == 0 && !do_e) {
        /* si no se encripta pero usuario eligió algoritmo, no pedimos clave */
    }

    /* Construir comando seguro (escapamos comillas simples) */
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "./gsea");
    if (do_c) strncat(cmd, " -c", sizeof(cmd)-strlen(cmd)-1);
    if (do_d) strncat(cmd, " -d", sizeof(cmd)-strlen(cmd)-1);
    if (do_e) strncat(cmd, " -e", sizeof(cmd)-strlen(cmd)-1);
    if (do_u) strncat(cmd, " -u", sizeof(cmd)-strlen(cmd)-1);

    char tmp[4096];
    snprintf(tmp, sizeof(tmp), " --comp-alg %s", comp_alg);
    strncat(cmd, tmp, sizeof(cmd)-strlen(cmd)-1);
    snprintf(tmp, sizeof(tmp), " --enc-alg %s", enc_alg);
    strncat(cmd, tmp, sizeof(cmd)-strlen(cmd)-1);

    /* Añadir -i y -o (entre comillas simples, con escape de ' ) */
    char in_esc[4096]={0}, out_esc[4096]={0};
    size_t p=0;
    in_esc[p++] = '\'';
    for (size_t i=0;i<strlen(in) && p+2<sizeof(in_esc);++i) {
        if (in[i]=='\'') { in_esc[p++]='\\'; in_esc[p++]='\''; } else in_esc[p++]=in[i];
    }
    in_esc[p++] = '\'';
    in_esc[p]=0;
    p=0;
    out_esc[p++] = '\'';
    for (size_t i=0;i<strlen(out) && p+2<sizeof(out_esc);++i) {
        if (out[i]=='\'') { out_esc[p++]='\\'; out_esc[p++]='\''; } else out_esc[p++]=out[i];
    }
    out_esc[p++] = '\'';
    out_esc[p]=0;

    strncat(cmd, " -i ", sizeof(cmd)-strlen(cmd)-1);
    strncat(cmd, in_esc, sizeof(cmd)-strlen(cmd)-1);
    strncat(cmd, " -o ", sizeof(cmd)-strlen(cmd)-1);
    strncat(cmd, out_esc, sizeof(cmd)-strlen(cmd)-1);

    if (key && do_e) {
        /* escapamos la clave de forma simple */
        char kesc[1024] = {'\0'};
        size_t kk=0; kesc[kk++] = '\'';
        for (size_t i=0;i<strlen(key) && kk+2<sizeof(kesc);++i) {
            if (key[i]=='\'') { kesc[kk++]='\\'; kesc[kk++]='\''; } else kesc[kk++]=key[i];
        }
        kesc[kk++] = '\''; kesc[kk]=0;
        strncat(cmd, " -k ", sizeof(cmd)-strlen(cmd)-1);
        strncat(cmd, kesc, sizeof(cmd)-strlen(cmd)-1);
    }

    printf("Comando a ejecutar:\n%s\n", cmd);
    if (!prompt_yesno("Confirmar y ejecutar? (y/N): ")) {
        puts("Cancelado.");
        free(in); free(out); if (key) free(key);
        return 0;
    }

    /* Ejecuta el mismo binario con las opciones seleccionadas */
    int rc = system(cmd);
    printf("Proceso finalizó con código %d\n", rc);

    free(in); free(out); if (key) free(key);
    return rc == -1 ? 1 : WEXITSTATUS(rc);
}
/* --------- fin menú interactivo --------- */
