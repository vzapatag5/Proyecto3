/**
 * Cifrado Vigenère muy simple sobre bytes.
 * Idea básica: se toma una clave (arreglo de bytes) y se repite tantas
 * veces como haga falta para cubrir el tamaño del buffer. Cada byte del
 * mensaje se "desplaza" sumando el byte correspondiente de la clave.
 * Para descifrar se hace la operación inversa (restar).
 *
 * NOTA IMPORTANTE: Esta versión es sólo educativa. No ofrece seguridad
 * real frente a ataques modernos. Se usa aquí como ejemplo sencillo.
 */
#include "vigenere.h"

// Cifra el contenido de buf "sumando" (mod 256) la clave repetida.
void vigenere_encrypt(uint8_t* buf, size_t len, const uint8_t* key, size_t key_len) {
    if (!key || key_len == 0) return; // Si no hay clave, no hacemos nada.
    for (size_t i = 0; i < len; ++i) {
        // i % key_len hace que la clave se repita cíclicamente.
        buf[i] = (uint8_t)((buf[i] + key[i % key_len]) & 0xFF);
    }
}

// Descifra haciendo la operación inversa: resta el mismo patrón repetido.
void vigenere_decrypt(uint8_t* buf, size_t len, const uint8_t* key, size_t key_len) {
    if (!key || key_len == 0) return;
    for (size_t i = 0; i < len; ++i) {
        buf[i] = (uint8_t)((buf[i] - key[i % key_len]) & 0xFF);
    }
}
