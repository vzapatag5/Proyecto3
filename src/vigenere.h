#ifndef VIGENERE_H
#define VIGENERE_H
#include <stddef.h>
#include <stdint.h>

void vigenere_encrypt(uint8_t* buf, size_t len, const uint8_t* key, size_t key_len);
void vigenere_decrypt(uint8_t* buf, size_t len, const uint8_t* key, size_t key_len);

#endif
