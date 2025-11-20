/*
 * ====================================================================
 * Cifrado AES-256-CBC usando OpenSSL
 * ====================================================================
 * 
 * Este archivo implementa cifrado y descifrado AES de forma simple.
 * Si OpenSSL no está instalado, compila versiones "dummy" que devuelven error.
 */

#include "aes_simple.h"
#include <stdlib.h>
#include <string.h>

/* ========== Versión SIN OpenSSL (solo compila, no funciona) ========== */
#ifdef NO_OPENSSL

/* Si no tienes OpenSSL instalado, estas funciones devuelven error (-1) */
int aes_encrypt_buffer(const uint8_t* in, size_t in_len,
                       const char* pass,
                       uint8_t** out, size_t* out_len) {
    (void)in; (void)in_len; (void)pass; (void)out; (void)out_len;
    return -1; /* AES no disponible: instalar libssl-dev y recompilar */
}

int aes_decrypt_buffer(const uint8_t* in, size_t in_len,
                       const char* pass,
                       uint8_t** out, size_t* out_len) {
    (void)in; (void)in_len; (void)pass; (void)out; (void)out_len;
    return -1; /* AES no disponible: instalar libssl-dev y recompilar */
}

/* ========== Versión CON OpenSSL (funcional) ========== */
#else

#include <openssl/evp.h>
#include <openssl/sha.h>

/*
 * Convierte la contraseña en clave (32 bytes) e IV (16 bytes)
 * 
 * AES necesita:
 * - Una clave de 256 bits (32 bytes)
 * - Un vector de inicialización IV de 128 bits (16 bytes)
 * 
 * Usamos SHA-256 para derivarlos desde la contraseña:
 * - Clave: SHA-256(contraseña)
 * - IV: primeros 16 bytes de SHA-256(contraseña + "GSEA-IV")
 */
static void derive_key_iv(const char* pass, uint8_t key[32], uint8_t iv[16]) {
    unsigned char d1[SHA256_DIGEST_LENGTH];  /* 32 bytes */
    unsigned char d2[SHA256_DIGEST_LENGTH];
    SHA256_CTX ctx;

    /* Generar clave: hash de la contraseña */
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, (const unsigned char*)pass, strlen(pass));
    SHA256_Final(d1, &ctx);
    memcpy(key, d1, 32);  /* Copiamos los 32 bytes completos */

    /* Generar IV: hash de (contraseña + "GSEA-IV"), tomamos 16 bytes */
    const char* salt_tag = "GSEA-IV";
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, (const unsigned char*)pass, strlen(pass));
    SHA256_Update(&ctx, (const unsigned char*)salt_tag, strlen(salt_tag));
    SHA256_Final(d2, &ctx);
    memcpy(iv, d2, 16);  /* Solo los primeros 16 bytes */
}

/*
 * Cifra un buffer usando AES-256-CBC
 * 
 * Parámetros:
 * - in: datos a cifrar
 * - in_len: tamaño de los datos
 * - pass: contraseña (se convierte en clave+IV)
 * - out: buffer de salida (se aloja aquí, el llamador debe liberar)
 * - out_len: tamaño del resultado cifrado
 * 
 * Retorna: 0 si OK, -1 si falla
 */
int aes_encrypt_buffer(const uint8_t* in, size_t in_len,
                       const char* pass,
                       uint8_t** out, size_t* out_len) {
    /* Validar entrada */
    if (!in || in_len == 0 || !pass || !out || !out_len) return -1;

    /* Derivar clave e IV desde la contraseña */
    uint8_t key[32], iv[16];
    derive_key_iv(pass, key, iv);

    /* Crear contexto de OpenSSL para cifrado */
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int rc = -1;
    int nout1 = 0, nout2 = 0;
    
    /* Reservar espacio: tamaño original + hasta 16 bytes de padding PKCS#7 */
    size_t cap = in_len + 16;
    uint8_t* buf = (uint8_t*)malloc(cap ? cap : 1);
    if (!buf) goto cleanup;

    /* Inicializar cifrado AES-256-CBC con la clave e IV */
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) 
        goto cleanup;
    
    /* Cifrar los datos (procesa en bloques de 16 bytes) */
    if (EVP_EncryptUpdate(ctx, buf, &nout1, in, (int)in_len) != 1) 
        goto cleanup;
    
    /* Finalizar: añade el padding y cifra el último bloque */
    if (EVP_EncryptFinal_ex(ctx, buf + nout1, &nout2) != 1) 
        goto cleanup;

    /* Guardar resultado */
    *out_len = (size_t)(nout1 + nout2);
    *out = buf;
    rc = 0; 
    buf = NULL;  /* No liberar: el llamador lo usará */

cleanup:
    if (buf) free(buf);
    EVP_CIPHER_CTX_free(ctx);
    
    /* Limpiar clave e IV de memoria por seguridad */
    OPENSSL_cleanse(key, sizeof(key));
    OPENSSL_cleanse(iv, sizeof(iv));
    return rc;
}

/*
 * Descifra un buffer usando AES-256-CBC
 * 
 * Parámetros:
 * - in: datos cifrados
 * - in_len: tamaño de los datos cifrados
 * - pass: contraseña (debe ser la misma que al cifrar)
 * - out: buffer de salida (se aloja aquí, el llamador debe liberar)
 * - out_len: tamaño del resultado descifrado (sin padding)
 * 
 * Retorna: 0 si OK, -1 si falla (ej: contraseña incorrecta, datos corruptos)
 */
int aes_decrypt_buffer(const uint8_t* in, size_t in_len,
                       const char* pass,
                       uint8_t** out, size_t* out_len) {
    /* Validar entrada */
    if (!in || in_len == 0 || !pass || !out || !out_len) return -1;

    /* Derivar clave e IV (deben coincidir con los del cifrado) */
    uint8_t key[32], iv[16];
    derive_key_iv(pass, key, iv);

    /* Crear contexto de OpenSSL para descifrado */
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int rc = -1;
    int nout1 = 0, nout2 = 0;
    
    /* Reservar espacio: máximo el tamaño de entrada (el padding se quita) */
    uint8_t* buf = (uint8_t*)malloc(in_len ? in_len : 1);
    if (!buf) goto cleanup;

    /* Inicializar descifrado AES-256-CBC */
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv) != 1) 
        goto cleanup;
    
    /* Descifrar los datos */
    if (EVP_DecryptUpdate(ctx, buf, &nout1, in, (int)in_len) != 1) 
        goto cleanup;
    
    /* Finalizar: verifica y quita el padding PKCS#7 */
    if (EVP_DecryptFinal_ex(ctx, buf + nout1, &nout2) != 1) 
        goto cleanup;

    /* Guardar resultado (ya sin padding) */
    *out_len = (size_t)(nout1 + nout2);
    *out = buf;
    rc = 0; 
    buf = NULL;  /* No liberar: el llamador lo usará */

cleanup:
    if (buf) free(buf);
    EVP_CIPHER_CTX_free(ctx);
    
    /* Limpiar clave e IV de memoria por seguridad */
    OPENSSL_cleanse(key, sizeof(key));
    OPENSSL_cleanse(iv, sizeof(iv));
    return rc;
}

#endif /* NO_OPENSSL */
