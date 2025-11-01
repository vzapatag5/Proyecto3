#include "vigenere.h"

// Cifrado Vigenère (byte a byte, con wrap-around)
void vigenere_encrypt(uint8_t* buf, size_t len, const uint8_t* key, size_t key_len) {
    if (!key || key_len == 0) return;
    for (size_t i = 0; i < len; ++i) {
        buf[i] = (uint8_t)((buf[i] + key[i % key_len]) & 0xFF);
    }
}
// Descifrado Vigenère (byte a byte, con wrap-around)
void vigenere_decrypt(uint8_t* buf, size_t len, const uint8_t* key, size_t key_len) {
    if (!key || key_len == 0) return;
    for (size_t i = 0; i < len; ++i) {
        buf[i] = (uint8_t)((buf[i] - key[i % key_len]) & 0xFF);
    }
}
