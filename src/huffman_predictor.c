#include "huffman_predictor.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_SYMBOLS 256

typedef struct Node {
    uint8_t sym;
    size_t freq;
    struct Node* left;
    struct Node* right;
} Node;

typedef struct {
    uint8_t* data;
    size_t bit_pos;
    size_t capacity;
} BitWriter;

typedef struct {
    const uint8_t* data;
    size_t bit_pos;
    size_t capacity;
} BitReader;

/* ----------------- Bit I/O helpers ----------------- */
static BitWriter* bw_create(size_t cap) {
    BitWriter* bw = malloc(sizeof(BitWriter));
    bw->data = calloc(cap, 1);
    bw->bit_pos = 0;
    bw->capacity = cap;
    return bw;
}

static void bw_write_bit(BitWriter* bw, int bit) {
    if (bw->bit_pos >= bw->capacity * 8) return;
    size_t byte_pos = bw->bit_pos / 8;
    int bit_offset = 7 - (bw->bit_pos % 8);
    if (bit) bw->data[byte_pos] |= (1 << bit_offset);
    bw->bit_pos++;
}

static void bw_write_byte(BitWriter* bw, uint8_t b) {
    for (int i = 7; i >= 0; i--)
        bw_write_bit(bw, (b >> i) & 1);
}

static BitReader* br_create(const uint8_t* data, size_t len) {
    BitReader* br = malloc(sizeof(BitReader));
    br->data = data;
    br->bit_pos = 0;
    br->capacity = len * 8;
    return br;
}

static int br_read_bit(BitReader* br) {
    if (br->bit_pos >= br->capacity) return -1;
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
    if (flag == 1) {
        uint8_t sym = 0;
        for (int i = 0; i < 8; i++) {
            int bit = br_read_bit(br);
            if (bit < 0) return NULL;
            sym = (sym << 1) | bit;
        }
        return new_node(sym, 0, NULL, NULL);
    } else if (flag == 0) {
        Node* left = deserialize_tree(br);
        Node* right = deserialize_tree(br);
        return new_node(0, 0, left, right);
    }
    return NULL;
}

static void build_codes(Node* n, char** tbl, char* pref, int depth) {
    if (!n) return;
    if (!n->left && !n->right) {
        pref[depth] = '\0';
        tbl[n->sym] = strdup(pref);
        return;
    }
    pref[depth] = '0'; build_codes(n->left, tbl, pref, depth + 1);
    pref[depth] = '1'; build_codes(n->right, tbl, pref, depth + 1);
}

/* ----------------- Predictor ----------------- */
static void apply_predictor(uint8_t* buf, size_t len) {
    uint8_t prev = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t cur = buf[i];
        buf[i] = cur - prev;
        prev = cur;
    }
}

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
    if (!in || len == 0) return -1;

    uint8_t* data = malloc(len);
    memcpy(data, in, len);
    apply_predictor(data, len);

    size_t freq[MAX_SYMBOLS] = {0};
    for (size_t i = 0; i < len; i++) freq[data[i]]++;

    Node* nodes[MAX_SYMBOLS];
    size_t n = 0;
    for (int i = 0; i < 256; i++) if (freq[i]) nodes[n++] = new_node(i, freq[i], NULL, NULL);

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

    BitWriter* bw = bw_create(len * 3);
    serialize_tree(root, bw);

    // escribir tamaño original
    for (int i = 31; i >= 0; i--)
        bw_write_bit(bw, (len >> i) & 1);

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
    if (!in || len == 0) return -1;

    BitReader* br = br_create(in, len);
    Node* root = deserialize_tree(br);
    if (!root) return -1;

    size_t orig_len = 0;
    for (int i = 0; i < 32; i++) {
        int bit = br_read_bit(br);
        if (bit < 0) return -1;
        orig_len = (orig_len << 1) | bit;
    }

    *out = malloc(orig_len);
    Node* cur = root;
    for (size_t i = 0; i < orig_len; i++) {
        while (cur->left || cur->right) {
            int bit = br_read_bit(br);
            if (bit < 0) break;
            cur = bit ? cur->right : cur->left;
        }
        (*out)[i] = cur->sym;
        cur = root;
    }

    undo_predictor(*out, orig_len);
    *out_len = orig_len;

    free_tree(root);
    free(br);
    return 0;
}
