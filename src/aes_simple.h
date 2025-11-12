#ifndef AES_SIMPLE_H
#define AES_SIMPLE_H

#include <stddef.h>
#include <stdint.h>

/* Cifra en AES-256-CBC con PKCS#7.
 * - pass: cadena cualquiera que pasas con -k
 * - in / in_len: datos de entrada
 * - out / out_len: salida allocada con malloc (el caller libera)
 * return 0 ok, !=0 error
 */
int aes_encrypt_buffer(const uint8_t* in, size_t in_len,
                       const char* pass,
                       uint8_t** out, size_t* out_len);

/* Descifra en AES-256-CBC con PKCS#7.
 * return 0 ok, !=0 error
 */
int aes_decrypt_buffer(const uint8_t* in, size_t in_len,
                       const char* pass,
                       uint8_t** out, size_t* out_len);

#endif
