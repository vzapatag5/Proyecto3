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

/* Evita warnings en algunos entornos */
extern int optind;
extern char *optarg;

/* Algoritmos soportados */
typedef enum { COMP_RLEVAR } CompAlg;
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
static char* build_tests_output_path(const char* out_path) {
    /* Recorta slashes finales para que -o "outdir/" => basename "outdir" */
    char* out_trim = trim_trailing_slashes(out_path);
    if (!out_trim) return NULL;

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

static int is_dir(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

static int is_regular(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode);
}

/* mkdir -p */
static int mkdirs(const char* path, mode_t mode) {
    if (!path || !*path) return -1;
    char* tmp = strdup(path);
    if (!tmp) return -1;

    size_t len = strlen(tmp);
    if (len == 0) { free(tmp); return -1; }
    if (tmp[len-1] == '/') tmp[len-1] = '\0';

    for (char* p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) { free(tmp); return -1; }
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) { free(tmp); return -1; }
    free(tmp);
    return 0;
}

/* Une rutas con '/' (malloc) */
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

/* Ruta absoluta simple */
static char* to_abs(const char* p) {
    if (!p) return NULL;
    if (p[0] == '/') return strdup(p);
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) return NULL;
    return join2(cwd, p);
}

/* tests/<basename(out_root)>/<relpath>, relpath = in_abs - prefijo in_root_abs */
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

/* ---------- Pipeline para UN archivo ---------- */
static int process_one_file(const char* in_path, const char* out_path, const Config* cfg) {
    uint8_t* buf = NULL; size_t len = 0;
    if (read_file(in_path, &buf, &len) != 0) {
        fprintf(stderr, "read_file fallo: %s\n", in_path);
        return -1;
    }

    uint8_t* tmp = NULL; size_t tlen = 0;

    /* FORWARD */
    if (cfg->do_c) {
        if (cfg->comp_alg == COMP_RLEVAR) {
            if (rle_var_compress(buf, len, &tmp, &tlen) != 0) {
                puts("RLE-Var: compresión falló");
                free(buf); return -1;
            }
            free(buf); buf = tmp; len = tlen;
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
            if (rle_var_decompress(buf, len, &tmp, &tlen) != 0) {
                puts("RLE-Var: descompresión falló");
                free(buf); return -1;
            }
            free(buf); buf = tmp; len = tlen;
        }
    }

    /* Asegurar directorio destino y escribir */
    char* out_dir = strdup(out_path);
    if (!out_dir) { free(buf); return -1; }
    char* slash = strrchr(out_dir, '/');
    if (slash) {
        *slash = '\0';
        if (mkdirs(out_dir, 0755) != 0) { perror("mkdirs"); free(out_dir); free(buf); return -1; }
    }
    free(out_dir);

    if (write_file(out_path, buf, len) != 0) {
        fprintf(stderr, "write_file fallo: %s\n", out_path);
        free(buf);
        return -1;
    }

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
