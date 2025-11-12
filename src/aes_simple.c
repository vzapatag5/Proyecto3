#include "aes_simple.h"
#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

/* Deriva key (32B) e iv (16B) desde pass usando SHA-256:
 *   key = SHA256(pass)
 *   iv  = SHA256(pass || "GSEA-IV") y se toman los primeros 16B
 * Es determinista y simple para laboratorio.
 */
static void derive_key_iv(const char* pass, uint8_t key[32], uint8_t iv[16]) {
    unsigned char d1[SHA256_DIGEST_LENGTH];
    unsigned char d2[SHA256_DIGEST_LENGTH];
    SHA256_CTX ctx;

    // key = SHA256(pass)
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, (const unsigned char*)pass, strlen(pass));
    SHA256_Final(d1, &ctx);
    memcpy(key, d1, 32);

    // iv = first 16 bytes of SHA256(pass || "GSEA-IV")
    const char* salt_tag = "GSEA-IV";
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, (const unsigned char*)pass, strlen(pass));
    SHA256_Update(&ctx, (const unsigned char*)salt_tag, strlen(salt_tag));
    SHA256_Final(d2, &ctx);
    memcpy(iv, d2, 16);
}

int aes_encrypt_buffer(const uint8_t* in, size_t in_len,
                       const char* pass,
                       uint8_t** out, size_t* out_len) {
    if (!in || in_len == 0 || !pass || !out || !out_len) return -1;

    uint8_t key[32], iv[16];
    derive_key_iv(pass, key, iv);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int rc = -1;
    int nout1 = 0, nout2 = 0;
    size_t cap = in_len + 16;                 // espacio extra para padding
    uint8_t* buf = (uint8_t*)malloc(cap ? cap : 1);
    if (!buf) goto cleanup;

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) goto cleanup;
    if (EVP_EncryptUpdate(ctx, buf, &nout1, in, (int)in_len) != 1) goto cleanup;
    if (EVP_EncryptFinal_ex(ctx, buf + nout1, &nout2) != 1) goto cleanup;

    *out_len = (size_t)(nout1 + nout2);
    *out = buf;
    rc = 0; buf = NULL;

cleanup:
    if (buf) free(buf);
    EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(key, sizeof(key));
    OPENSSL_cleanse(iv, sizeof(iv));
    return rc;
}

int aes_decrypt_buffer(const uint8_t* in, size_t in_len,
                       const char* pass,
                       uint8_t** out, size_t* out_len) {
    if (!in || in_len == 0 || !pass || !out || !out_len) return -1;

    uint8_t key[32], iv[16];
    derive_key_iv(pass, key, iv);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int rc = -1;
    int nout1 = 0, nout2 = 0;
    uint8_t* buf = (uint8_t*)malloc(in_len ? in_len : 1); // tamaño max sin padding
    if (!buf) goto cleanup;

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) goto cleanup;
    if (EVP_DecryptUpdate(ctx, buf, &nout1, in, (int)in_len) != 1) goto cleanup;
    if (EVP_DecryptFinal_ex(ctx, buf + nout1, &nout2) != 1) goto cleanup;

    *out_len = (size_t)(nout1 + nout2);
    *out = buf;
    rc = 0; buf = NULL;

cleanup:
    if (buf) free(buf);
    EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(key, sizeof(key));
    OPENSSL_cleanse(iv, sizeof(iv));
    return rc;
}
