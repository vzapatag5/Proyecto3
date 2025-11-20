// ============================================================================
// Huffman con predictor de diferencias (delta entre bytes consecutivos)
// ----------------------------------------------------------------------------
// Flujo de compresión:
// 1. Copiamos la entrada y aplicamos un predictor simple: reemplaza cada byte
//    por (actual - anterior). Esto hace que datos suaves/graudales generen
//    muchos valores pequeños repetidos (mejora las frecuencias para Huffman).
// 2. Contamos frecuencias de cada valor (0..255).
// 3. Construimos el árbol Huffman (combinando nodos de menor frecuencia).
// 4. Generamos las cadenas de bits (códigos) para cada símbolo hoja.
// 5. Serializamos el árbol (preorden) + escribimos la longitud original + los
//    bits codificados de la secuencia transformada.
// Flujo de descompresión:
// 1. Leemos y reconstruimos el árbol.
// 2. Leemos la longitud original.
// 3. Decodificamos símbolo por símbolo usando el árbol.
// 4. Aplicamos el predictor inverso para volver a los bytes originales.
// Nota: El BitWriter reserva len*3 bytes como tamaño aproximado; en casos
//       extremos (datos muy incomprimibles) podría quedarse corto. Se podría
//       mejorar con realocación dinámica.
// ============================================================================

#include "huffman_predictor.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_SYMBOLS 256

// Nodo del árbol Huffman: si es hoja 'sym' tiene el símbolo; si no, se usa
// como nodo interno y 'sym' no es relevante. 'freq' es la suma de frecuencias.
typedef struct Node {
    uint8_t sym;
    size_t freq;
    struct Node* left;
    struct Node* right;
} Node;

// BitWriter: escribe bits uno a uno sobre un buffer de bytes.
// 'bit_pos' indica cuántos bits se han usado ya.
typedef struct {
    uint8_t* data;
    size_t bit_pos;
    size_t capacity; // en bytes
} BitWriter;

// BitReader: permite leer bits secuencialmente de un buffer de entrada.
typedef struct {
    const uint8_t* data;
    size_t bit_pos;
    size_t capacity; // en bits (capacity = len * 8)
} BitReader;

/* ----------------- Bit I/O helpers ----------------- */
static BitWriter* bw_create(size_t cap) {
    BitWriter* bw = malloc(sizeof(BitWriter));
    bw->data = calloc(cap, 1); // inicializa en cero para evitar basura en bits
    bw->bit_pos = 0;
    bw->capacity = cap;
    return bw;
}

// Escribe un único bit (0 o 1). Si se llena la capacidad, se ignora el bit.
static void bw_write_bit(BitWriter* bw, int bit) {
    if (bw->bit_pos >= bw->capacity * 8) return; // sin espacio (optimización simple)
    size_t byte_pos = bw->bit_pos / 8;
    int bit_offset = 7 - (bw->bit_pos % 8); // orden: bit más significativo primero
    if (bit) bw->data[byte_pos] |= (1 << bit_offset);
    bw->bit_pos++;
}

// Escribe los 8 bits de un byte (de más significativo a menos significativo).
static void bw_write_byte(BitWriter* bw, uint8_t b) {
    for (int i = 7; i >= 0; i--)
        bw_write_bit(bw, (b >> i) & 1);
}

static BitReader* br_create(const uint8_t* data, size_t len) {
    BitReader* br = malloc(sizeof(BitReader));
    br->data = data;
    br->bit_pos = 0;
    br->capacity = len * 8; // capacidad en bits
    return br;
}

// Devuelve el siguiente bit (0/1) o -1 si ya no hay más.
static int br_read_bit(BitReader* br) {
    if (br->bit_pos >= br->capacity) return -1; // fin de datos
    size_t byte_pos = br->bit_pos / 8;
    int bit_offset = 7 - (br->bit_pos % 8);
    int bit = (br->data[byte_pos] >> bit_offset) & 1;
    br->bit_pos++;
    return bit;
}

/* ----------------- Huffman core ----------------- */

static Node* new_node(uint8_t s, size_t f, Node* l, Node* r) {
    Node* n = malloc(sizeof(Node));
    n->sym = s;
    n->freq = f;
    n->left = l;
    n->right = r;
    return n;
}

static void free_tree(Node* n) {
    if (!n) return;
    free_tree(n->left);
    free_tree(n->right);
    free(n);
}

/* serializa el árbol (preorden) para reconstrucción */
// Serializa el árbol en preorden:
// bit 1 seguido del símbolo para hojas, bit 0 para nodos internos.
static void serialize_tree(Node* n, BitWriter* bw) {
    if (!n) return;
    if (!n->left && !n->right) {
        bw_write_bit(bw, 1);       // hoja
        bw_write_byte(bw, n->sym); // símbolo
    } else {
        bw_write_bit(bw, 0);       // nodo interno
        serialize_tree(n->left, bw);
        serialize_tree(n->right, bw);
    }
}

static Node* deserialize_tree(BitReader* br) {
    int flag = br_read_bit(br);
    if (flag == 1) { // hoja: leer el byte asociado
        uint8_t sym = 0;
        for (int i = 0; i < 8; i++) {
            int bit = br_read_bit(br);
            if (bit < 0) return NULL;
            sym = (sym << 1) | bit;
        }
        return new_node(sym, 0, NULL, NULL);
    } else if (flag == 0) { // nodo interno: reconstruir recursivamente
        Node* left = deserialize_tree(br);
        Node* right = deserialize_tree(br);
        return new_node(0, 0, left, right);
    }
    return NULL;
}

// Recorre el árbol y genera la cadena de bits (como texto '0'/'1') para cada símbolo hoja.
static void build_codes(Node* n, char** tbl, char* pref, int depth) {
    if (!n) return;
    if (!n->left && !n->right) {
        pref[depth] = '\0';
        tbl[n->sym] = strdup(pref); // copia el código encontrado
        return;
    }
    pref[depth] = '0'; build_codes(n->left, tbl, pref, depth + 1);
    pref[depth] = '1'; build_codes(n->right, tbl, pref, depth + 1);
}

/* ----------------- Predictor ----------------- */
// Predictor: reemplaza cada byte por la diferencia respecto al anterior.
static void apply_predictor(uint8_t* buf, size_t len) {
    uint8_t prev = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t cur = buf[i];
        buf[i] = cur - prev; // delta
        prev = cur;
    }
}

// Inverso del predictor: reconstruye los valores originales sumando acumulativamente.
static void undo_predictor(uint8_t* buf, size_t len) {
    uint8_t prev = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t val = buf[i] + prev;
        buf[i] = val;
        prev = val;
    }
}

/* ----------------- Compresión ----------------- */
int hp_compress_buffer(const uint8_t* in, size_t len, uint8_t** out, size_t* out_len) {
    if (!in || len == 0) return -1; // nada que comprimir

    uint8_t* data = malloc(len);
    memcpy(data, in, len);
    apply_predictor(data, len);

    // Contar frecuencias de cada símbolo (post-predictor)
    size_t freq[MAX_SYMBOLS] = {0};
    for (size_t i = 0; i < len; i++) freq[data[i]]++;

    Node* nodes[MAX_SYMBOLS];
    size_t n = 0;
    for (int i = 0; i < 256; i++) if (freq[i]) nodes[n++] = new_node(i, freq[i], NULL, NULL);

    // Combinar siempre los dos nodos de menor frecuencia hasta quedar uno
    while (n > 1) {
        size_t i1 = 0, i2 = 1;
        if (nodes[i2]->freq < nodes[i1]->freq) { i1 = 1; i2 = 0; }
        for (size_t i = 2; i < n; i++) {
            if (nodes[i]->freq < nodes[i1]->freq) { i2 = i1; i1 = i; }
            else if (nodes[i]->freq < nodes[i2]->freq) i2 = i;
        }
        Node* a = nodes[i1]; Node* b = nodes[i2];
        Node* parent = new_node(0, a->freq + b->freq, a, b);
        nodes[i1] = parent;
        nodes[i2] = nodes[n - 1];
        n--;
    }
    Node* root = (n == 1) ? nodes[0] : new_node(0, 1, NULL, NULL);

    char* tbl[256] = {0};
    char tmp[256]; build_codes(root, tbl, tmp, 0);

    BitWriter* bw = bw_create(len * 3); // capacidad aproximada (heurística simple)
    serialize_tree(root, bw);

    // Guardar longitud original (32 bits) para saber cuánto reconstruir luego
    for (int i = 31; i >= 0; i--)
        bw_write_bit(bw, (len >> i) & 1);

    // Codificar cada símbolo usando su secuencia de bits
    for (size_t i = 0; i < len; i++) {
        const char* code = tbl[data[i]];
        for (size_t j = 0; j < strlen(code); j++)
            bw_write_bit(bw, code[j] == '1');
    }

    *out = malloc((bw->bit_pos + 7) / 8);
    memcpy(*out, bw->data, (bw->bit_pos + 7) / 8);
    *out_len = (bw->bit_pos + 7) / 8;

    for (int i = 0; i < 256; i++) free(tbl[i]);
    free_tree(root);
    free(bw->data); free(bw);
    free(data);
    return 0;
}

/* ----------------- Descompresión ----------------- */
int hp_decompress_buffer(const uint8_t* in, size_t len, uint8_t** out, size_t* out_len) {
    if (!in || len == 0) return -1; // nada que leer

    BitReader* br = br_create(in, len);
    Node* root = deserialize_tree(br);
    if (!root) return -1;

    // Leer la longitud original (32 bits)
    size_t orig_len = 0;
    for (int i = 0; i < 32; i++) {
        int bit = br_read_bit(br);
        if (bit < 0) return -1;
        orig_len = (orig_len << 1) | bit;
    }

    *out = malloc(orig_len);
    Node* cur = root;
    // Decodificar símbolo por símbolo recorriendo el árbol según los bits
    for (size_t i = 0; i < orig_len; i++) {
        while (cur->left || cur->right) { // mientras no sea hoja
            int bit = br_read_bit(br);
            if (bit < 0) break; // datos truncados
            cur = bit ? cur->right : cur->left;
        }
        (*out)[i] = cur->sym;
        cur = root; // reiniciar para el siguiente símbolo
    }

    undo_predictor(*out, orig_len); // revertir delta para recuperar datos originales
    *out_len = orig_len;

    free_tree(root);
    free(br);
    return 0;
}
