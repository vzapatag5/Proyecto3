#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 199309L
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

#define MAGIC_HDR     "GSEAIMG1"
#define MAGIC_HDR_LEN 8
#define FMT_PNG  1
#define FMT_JPEG 2

extern int optind;
extern char *optarg;

/* ---------- Algoritmos ---------- */
typedef enum {
    COMP_RLEVAR,
    COMP_LZW,
    COMP_LZWPRED,
    COMP_HUFFMANPRED
} CompAlg;

typedef enum { ENC_NONE, ENC_VIG, ENC_AES } EncAlg;

typedef struct {
    int do_c, do_d, do_e, do_u;
    const char* in_path;
    const char* out_path;
    const char* key;
    CompAlg comp_alg;
    EncAlg  enc_alg;
} Config;

/* ---------- Prototipos ---------- */
static int ensure_tests_dir(void);
static char* build_tests_output_path(const char* out_path);
static char* join2(const char* a, const char* b);
static int run_interactive(void);
static char* prompt_line(const char* prompt);
static void human_readable(size_t bytes, char* out, size_t out_size);
static void csv_quote(FILE* f, const char* s);
static void apply_predictor_sub(uint8_t* buf, int w, int h, int ch);
static void undo_predictor_sub(uint8_t* buf, int w, int h, int ch);

/* ---------- Predictor ---------- */
static void apply_predictor_sub(uint8_t* buf, int w, int h, int ch) {
    if (!buf || w <= 0 || h <= 0 || ch <= 0) return;
    for (int y = 0; y < h; ++y) {
        size_t row_off = (size_t)y * w * ch;
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

/* ---------- Utilidades FS mínimas que usa este main ---------- */
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

static char* build_tests_output_path(const char* out_path) {
    if (!out_path) return NULL;
    size_t n = strlen(out_path);
    char* out_trim = (char*)malloc(n + 1);
    if (!out_trim) return NULL;
    memcpy(out_trim, out_path, n + 1);
    while (n > 1 && out_trim[n-1] == '/') out_trim[--n] = '\0';

    if (strncmp(out_trim, "tests/", 6) == 0 || strcmp(out_trim, "tests") == 0) {
        return out_trim; /* ya está dentro de tests/ */
    }

    const char* base = strrchr(out_trim, '/');
#ifdef _WIN32
    const char* b2 = strrchr(out_trim, '\\');
    if (!base || (b2 && b2 > base)) base = b2;
#endif
    base = base ? base + 1 : out_trim;
    if (*base == '\0') base = "out";

    char* dst = join2("tests", base);
    free(out_trim);
    return dst;
}

/* ---------- Parser CLI ---------- */
static int parse_args(int argc, char* argv[], Config* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->comp_alg = COMP_RLEVAR;
    cfg->enc_alg  = ENC_VIG;

    static struct option long_opts[] = {
        {"comp-alg", required_argument, 0, 1},
        {"enc-alg",  required_argument, 0, 2},
        {0,0,0,0}
    };

    int opt, idx=0;
    while ((opt = getopt_long(argc, argv, "cdeui:o:k:", long_opts, &idx)) != -1) {
        switch (opt) {
            case 'c': cfg->do_c = 1; break;
            case 'd': cfg->do_d = 1; break;
            case 'e': cfg->do_e = 1; break;
            case 'u': cfg->do_u = 1; break;
            case 'i': cfg->in_path = optarg; break;
            case 'o': cfg->out_path = optarg; break;
            case 'k': cfg->key = optarg; break;
            case 1:
                if      (strcmp(optarg,"rlevar")==0)       cfg->comp_alg = COMP_RLEVAR;
                else if (strcmp(optarg,"lzw")==0)          cfg->comp_alg = COMP_LZW;
                else if (strcmp(optarg,"lzw-pred")==0)     cfg->comp_alg = COMP_LZWPRED;
                else if (strcmp(optarg,"huffman-pred")==0) cfg->comp_alg = COMP_HUFFMANPRED;
                else { fprintf(stderr,"Compresión desconocida: %s\n",optarg); return -1; }
                break;
            case 2:
                if      (strcmp(optarg,"vigenere")==0) cfg->enc_alg = ENC_VIG;
                else if (strcmp(optarg,"aes")==0)      cfg->enc_alg = ENC_AES;
                else if (strcmp(optarg,"none")==0)     cfg->enc_alg = ENC_NONE;
                else { fprintf(stderr,"Cifrado desconocido: %s\n",optarg); return -1; }
                break;
            default:
                return -1;
        }
    }

    if (!cfg->in_path || !cfg->out_path) {
        fprintf(stderr,"Debe indicar -i entrada y -o salida\n");
        return -1;
    }
    if ((cfg->do_e || cfg->do_u) && cfg->enc_alg != ENC_NONE) {
        if (!cfg->key || cfg->key[0] == '\0') {
            fprintf(stderr,"Debe indicar clave con -k cuando usa -e/-u y --enc-alg distinto de 'none'\n");
            return -1;
        }
    }
    return 0;
}

/* ---------- Utilidades ---------- */
static char* join2(const char* a, const char* b) {
    if (!a || !b) return NULL;
    size_t na = strlen(a), nb = strlen(b);
    int need_slash = (na>0 && a[na-1]!='/');
    char* s = (char*)malloc(na + nb + (need_slash?2:1));
    if (!s) return NULL;
    strcpy(s,a);
    if (need_slash) strcat(s,"/");
    strcat(s,b);
    return s;
}

static void human_readable(size_t bytes, char* out, size_t out_size) {
    const char* u[]={"B","KB","MB","GB","TB"};
    double v=(double)bytes; int i=0;
    while(v>=1024 && i<4){v/=1024;i++;}
    snprintf(out,out_size,"%.2f%s",v,u[i]);
}

static void csv_quote(FILE* f, const char* s) {
    fputc('"',f);
    if(s){for(const char*p=s;*p;p++){ if(*p=='"') fputc('"',f); fputc(*p,f);} }
    fputc('"',f);
}

static int is_dir(const char* p){struct stat st;return (stat(p,&st)==0 && S_ISDIR(st.st_mode));}
static int is_regular(const char* p){struct stat st;return (stat(p,&st)==0 && S_ISREG(st.st_mode));}

/* ---------- Procesamiento de un archivo ---------- */
static int process_one_file(const char* in, const char* out, const Config* cfg,
                            size_t* o_orig, size_t* o_fin, double* o_ms) {
    uint8_t* buf=NULL; size_t len=0;
    if(read_file(in,&buf,&len)!=0){fprintf(stderr,"Error al leer %s\n",in);return -1;}
    if (o_orig) *o_orig=len;

    uint8_t* tmp=NULL; size_t tlen=0;
    struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);

    /* ---- FORWARD ---- */
    if(cfg->do_c){
        if(cfg->comp_alg==COMP_RLEVAR){
            if (rle_var_compress(buf,len,&tmp,&tlen)!=0){fprintf(stderr,"RLE-Var falló\n"); free(buf); return -1;}
        }else if(cfg->comp_alg==COMP_LZW){
            if (lzw_compress(buf,len,&tmp,&tlen)!=0){fprintf(stderr,"LZW falló\n"); free(buf); return -1;}
        }else if(cfg->comp_alg==COMP_LZWPRED){
            /* OJO: en este main "simple" no tenemos w/h/ch; si quieres predictor real de imagen,
               pásalos aquí. Por ahora usamos (w/h/ch)=dummy para mantener la interfaz. */
            apply_predictor_sub(buf,1,1,1);
            if (lzw_compress(buf,len,&tmp,&tlen)!=0){fprintf(stderr,"LZW-PRED falló\n"); free(buf); return -1;}
        }else if(cfg->comp_alg==COMP_HUFFMANPRED){
            apply_predictor_sub(buf,1,1,1);
            if (hp_compress_buffer(buf,len,&tmp,&tlen)!=0){fprintf(stderr,"HUFFMAN-PRED falló\n"); free(buf); return -1;}
        }
        free(buf); buf=tmp; len=tlen; tmp=NULL; tlen=0;
    }

    if(cfg->do_e){
        if(cfg->enc_alg==ENC_VIG){
            vigenere_encrypt(buf,len,(const uint8_t*)cfg->key,strlen(cfg->key));
        }else if(cfg->enc_alg==ENC_AES){
            if (aes_encrypt_buffer(buf,len,cfg->key,&tmp,&tlen)!=0){
                fprintf(stderr,"AES: cifrado falló\n"); free(buf); return -1;
            }
            free(buf); buf=tmp; len=tlen; tmp=NULL; tlen=0;
        }
    }

    /* ---- BACKWARD ---- */
    if(cfg->do_u){
        if(cfg->enc_alg==ENC_VIG){
            vigenere_decrypt(buf,len,(const uint8_t*)cfg->key,strlen(cfg->key));
        }else if(cfg->enc_alg==ENC_AES){
            if (aes_decrypt_buffer(buf,len,cfg->key,&tmp,&tlen)!=0){
                fprintf(stderr,"AES: descifrado falló (clave o datos)\n"); free(buf); return -1;
            }
            free(buf); buf=tmp; len=tlen; tmp=NULL; tlen=0;
        }
    }

    if(cfg->do_d){
        if(cfg->comp_alg==COMP_RLEVAR){
            if (rle_var_decompress(buf,len,&tmp,&tlen)!=0){fprintf(stderr,"RLE-Var decomp falló\n"); free(buf); return -1;}
        }else if(cfg->comp_alg==COMP_LZW || cfg->comp_alg==COMP_LZWPRED){
            if (lzw_decompress(buf,len,&tmp,&tlen)!=0){fprintf(stderr,"LZW decomp falló\n"); free(buf); return -1;}
        }else if(cfg->comp_alg==COMP_HUFFMANPRED){
            if (hp_decompress_buffer(buf,len,&tmp,&tlen)!=0){fprintf(stderr,"HUFFMAN-PRED decomp falló\n"); free(buf); return -1;}
        }
        free(buf); buf=tmp; len=tlen; tmp=NULL; tlen=0;

        if(cfg->comp_alg==COMP_LZWPRED||cfg->comp_alg==COMP_HUFFMANPRED){
            undo_predictor_sub(buf,1,1,1);
        }
    }

    int wres = write_file(out,buf,len);

    clock_gettime(CLOCK_MONOTONIC,&t1);
    double ms=(t1.tv_sec-t0.tv_sec)*1000.0+(t1.tv_nsec-t0.tv_nsec)/1e6;
    if (o_fin) *o_fin=len; if (o_ms) *o_ms=ms;

    free(buf);
    return wres;
}

/* ---------- Tareas e hilos ---------- */
typedef struct {
    char* in;
    char* out;
    const Config* cfg;
    int res;
    size_t orig,fin;
    double ms;
} Task;

static void* worker(void* arg){
    Task*t=(Task*)arg;
    t->res=process_one_file(t->in,t->out,t->cfg,&t->orig,&t->fin,&t->ms);
    return NULL;
}

/* ---------- Recolección de archivos ---------- */
typedef struct {
    char** in; char** out;
    size_t n,cap;
} PathList;

static void pl_init(PathList* p){p->in=p->out=NULL;p->n=p->cap=0;}
static void pl_free(PathList* p){for(size_t i=0;i<p->n;i++){free(p->in[i]);free(p->out[i]);}free(p->in);free(p->out);}
static int pl_push(PathList* p,char*i,char*o){
    if(p->n==p->cap){
        size_t c=p->cap?p->cap*2:64;
        char** ni=(char**)realloc(p->in,c*sizeof(char*));
        char** no=(char**)realloc(p->out,c*sizeof(char*));
        if(!ni||!no){free(ni);free(no);return -1;}
        p->in=ni;p->out=no;p->cap=c;
    }
    p->in[p->n]=i;p->out[p->n]=o;p->n++;return 0;
}

/* ---------- Menú interactivo ---------- */
static const char* choose_comp_alg_str(void){
    printf("Elige algoritmo de compresión:\n");
    printf(" 1) rlevar\n 2) lzw\n 3) lzw-pred\n 4) huffman-pred\n");
    char buf[8]; if(!fgets(buf,sizeof(buf),stdin)) return "rlevar";
    int v=atoi(buf);
    if(v==2)return"lzw";if(v==3)return"lzw-pred";if(v==4)return"huffman-pred";
    return"rlevar";
}
static const char* choose_enc_alg_str(void){
    printf("Elige algoritmo de cifrado:\n");
    printf(" 1) vigenere\n 2) aes\n 3) none\n");
    char buf[8]; if(!fgets(buf,sizeof(buf),stdin)) return "vigenere";
    int v=atoi(buf);
    if(v==2) return "aes";
    if(v==3) return "none";
    return "vigenere";
}

static char* prompt_line(const char* p){
    printf("%s",p?p:"> ");fflush(stdout);
    char b[512];if(!fgets(b,sizeof(b),stdin))return NULL;
    size_t n=strlen(b);while(n&& (b[n-1]=='\n'||b[n-1]=='\r'))b[--n]='\0';
    return n?strdup(b):NULL;
}

/* ---------- main ---------- */
int main(int argc,char*argv[]){
    if(argc==1) return run_interactive();

    Config cfg;
    if(parse_args(argc,argv,&cfg)!=0) return 1;

    /* Si -o apunta fuera de tests/, replica tu política antigua de forzar tests/<basename> */
    if (ensure_tests_dir()!=0) return 1;
    char* out_fixed = build_tests_output_path(cfg.out_path);
    if (!out_fixed) { fprintf(stderr,"No se pudo preparar salida\n"); return 1; }

    int multi=is_dir(cfg.in_path);
    if(!multi){
        size_t o=0,f=0;double ms=0;
        if(process_one_file(cfg.in_path,out_fixed,&cfg,&o,&f,&ms)!=0){ free(out_fixed); return 1; }
        char oh[32],fh[32];human_readable(o,oh,sizeof(oh));human_readable(f,fh,sizeof(f));
        double a=(o? (1.0-(double)f/o)*100.0:0.0);
        printf("\nArchivo | Orig | Final | Ahorro(%%) | Tiempo(ms)\n");
        printf("-----------------------------------------------\n");
        printf("%-20s %10zu(%6s) %10zu(%6s) %7.2f%% %10.3f\n",
               cfg.in_path,o,oh,f,fh,a,ms);
        printf(">> Salida: %s\n", out_fixed);
        free(out_fixed);
        return 0;
    }

    /* Directorio: emparejar archivos de primer nivel (simple) */
    DIR*d=opendir(cfg.in_path);
    if(!d){perror("opendir");free(out_fixed);return 1;}
    PathList pl;pl_init(&pl);
    struct dirent*de;
    while((de=readdir(d))){
        if(!strcmp(de->d_name,".")||!strcmp(de->d_name,".."))continue;
        char*in=join2(cfg.in_path,de->d_name);
        char*out=join2(out_fixed,de->d_name);
        if(is_regular(in))pl_push(&pl,in,out);
        else { free(in); free(out); }
    }
    closedir(d);

    /* Crear carpeta tests/<basename(o)> si no existe */
    struct stat st;
    if (stat(out_fixed,&st)!=0){
        if (mkdir(out_fixed,0755)!=0 && errno!=EEXIST){
            perror("mkdir salida"); free(out_fixed); pl_free(&pl); return 1;
        }
    }
    free(out_fixed);

    pthread_t*ths=(pthread_t*)malloc(pl.n*sizeof(pthread_t));
    Task*t=(Task*)calloc(pl.n,sizeof(Task));
    for(size_t i=0;i<pl.n;i++){t[i].in=pl.in[i];t[i].out=pl.out[i];t[i].cfg=&cfg;
        pthread_create(&ths[i],NULL,worker,&t[i]);}
    for(size_t i=0;i<pl.n;i++)pthread_join(ths[i],NULL);

    printf("\nArchivo | Orig | Final | Ahorro(%%) | Tiempo(ms)\n");
    printf("-----------------------------------------------\n");
    FILE*fout=fopen("tests/gsea_results.csv","w");
    if(fout)fprintf(fout,"archivo,orig_bytes,final_bytes,ahorro_pct,tiempo_ms\n");
    size_t to=0,tf=0;double tt=0;
    for(size_t i=0;i<pl.n;i++){
        char oh[32],fh[32];human_readable(t[i].orig,oh,sizeof(oh));human_readable(t[i].fin,fh,sizeof(fh));
        double a=(t[i].orig?(1.0-(double)t[i].fin/t[i].orig)*100.0:0);
        printf("%-20s %10zu(%6s) %10zu(%6s) %7.2f%% %10.3f\n",
               t[i].in,t[i].orig,oh,t[i].fin,fh,a,t[i].ms);
        if(fout){
            csv_quote(fout,t[i].in); fprintf(fout,",%zu,%zu,%.2f,%.3f\n",
                t[i].orig,t[i].fin,a,t[i].ms);
        }
        to+=t[i].orig;tf+=t[i].fin;tt+=t[i].ms;
    }
    if(fout)fclose(fout);
    printf("-----------------------------------------------\n");
    double ah=(to?(1.0-(double)tf/to)*100.0:0);
    printf("TOTAL %12zu -> %12zu  Ahorro %.2f%%  Tiempo %.3f ms\n",to,tf,ah,tt);

    free(ths);free(t);pl_free(&pl);
    return 0;
}

/* ---------- Menú interactivo ---------- */
static int run_interactive(void){
    printf("Modo interactivo GSEA\n");
    if (ensure_tests_dir()!=0) return 1;

    char*in=prompt_line("Ruta de entrada: ");
    if(!in) return 1;
    char*out=prompt_line("Ruta de salida: ");
    if(!out){ free(in); return 1; }

    /* Forzar salida bajo tests/<basename(out)> */
    char* out_fixed = build_tests_output_path(out);
    if (!out_fixed){ free(in); free(out); return 1; }

    printf("Operación:\n 1) Comprimir\n 2) Encriptar\n 3) Comprimir+Encriptar\n 4) Descomprimir\n 5) Desencriptar\n 6) Descomprimir+Desencriptar\n");
    char*b=prompt_line("Opción: ");int op=atoi(b?b:"1");free(b);

    const char*comp=NULL;const char*enc=NULL;char*key=NULL;
    if(op==1||op==3||op==4||op==6)comp=choose_comp_alg_str();
    if(op==2||op==3||op==5||op==6){
        enc=choose_enc_alg_str();
        if(strcmp(enc,"none")!=0)key=prompt_line("Clave: ");
    }

    char cmd[2048]="./gsea";
    if(op==1||op==3)strcat(cmd," -c");
    if(op==2||op==3)strcat(cmd," -e");
    if(op==4||op==6)strcat(cmd," -d");
    if(op==5||op==6)strcat(cmd," -u");

    if(comp){strcat(cmd," --comp-alg ");strcat(cmd,comp);}
    if(enc){strcat(cmd," --enc-alg ");strcat(cmd,enc);}
    if(key){strcat(cmd," -k ");strcat(cmd,key);}

    strcat(cmd," -i ");strcat(cmd,in);
    strcat(cmd," -o ");strcat(cmd,out_fixed);
    printf("Ejecutando: %s\n",cmd);
    int rc=system(cmd);

    free(in);free(out);free(out_fixed);
    if(key) free(key);
    return rc==-1?1:WEXITSTATUS(rc);
}
/* ---------- Fin del archivo ---------- */
