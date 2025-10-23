#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "fs.h"
#include "rle.h"
#include "xor.h"

typedef struct {
    int do_c, do_d, do_e, do_u;
    const char* in_path;
    const char* out_path;
    const char* key;
} Config;

static int parse_args(int argc, char* argv[], Config* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    for (int i = 1; i < argc; ++i) {
        if      (strcmp(argv[i], "-c") == 0) cfg->do_c = 1;
        else if (strcmp(argv[i], "-d") == 0) cfg->do_d = 1;
        else if (strcmp(argv[i], "-e") == 0) cfg->do_e = 1;
        else if (strcmp(argv[i], "-u") == 0) cfg->do_u = 1;
        else if (strcmp(argv[i], "-i") == 0 && i+1 < argc) cfg->in_path = argv[++i];
        else if (strcmp(argv[i], "-o") == 0 && i+1 < argc) cfg->out_path = argv[++i];
        else if (strcmp(argv[i], "-k") == 0 && i+1 < argc) cfg->key = argv[++i];
        else { printf("Argumento desconocido o faltan valores: %s\n", argv[i]); return -1; }
    }
    if (!cfg->in_path || !cfg->out_path) return -1;
    if ((cfg->do_e || cfg->do_u) && !cfg->key) { puts("Error: -e/-u requieren -k <clave>"); return -1; }
    return 0;
}

static void print_help(void) {
    puts("Uso: ./gsea [-c|-d] [-e|-u] -i <entrada> -o <salida> [-k <clave>]");
    puts("Ejemplos:");
    puts("  ./gsea -c -e -i tests/a.txt -o out.bin -k 1234");
    puts("  ./gsea -u -d -i out.bin -o back.txt -k 1234");
}

int main(int argc, char* argv[]) {
    Config cfg;
    if (parse_args(argc, argv, &cfg) != 0) { print_help(); return 1; }

    uint8_t* buf = NULL; size_t len = 0;
    if (read_file(cfg.in_path, &buf, &len) != 0) { perror("read_file"); return 1; }

    uint8_t* tmp = NULL; size_t tlen = 0;

    if (cfg.do_c) {
        if (rle_compress(buf, len, &tmp, &tlen) != 0) { puts("RLE compress falló"); free(buf); return 1; }
        free(buf); buf = tmp; len = tlen;
    }
    if (cfg.do_e) {
        xor_apply(buf, len, (const uint8_t*)cfg.key, strlen(cfg.key));
    }

    if (cfg.do_u) {
        xor_apply(buf, len, (const uint8_t*)cfg.key, strlen(cfg.key));
    }
    if (cfg.do_d) {
        if (rle_decompress(buf, len, &tmp, &tlen) != 0) { puts("RLE decompress falló"); free(buf); return 1; }
        free(buf); buf = tmp; len = tlen;
    }

    if (write_file(cfg.out_path, buf, len) != 0) { perror("write_file"); free(buf); return 1; }
    free(buf);
    puts("Listo ✅");
    return 0;
}
