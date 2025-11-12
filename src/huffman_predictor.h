#ifndef HUFFMAN_PREDICTOR_H
#define HUFFMAN_PREDICTOR_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Comprime un buffer usando Huffman con predictor simple (tipo PNG SUB).
 * 
 * @param in        Buffer de entrada
 * @param in_len    Longitud del buffer de entrada
 * @param out       (Salida) Puntero a buffer comprimido (malloc)
 * @param out_len   (Salida) Tamaño del buffer comprimido
 * @return 0 si tuvo éxito, -1 si falló
 */
int hp_compress_buffer(const uint8_t* in, size_t in_len, uint8_t** out, size_t* out_len);

/**
 * @brief Descomprime un buffer comprimido con hp_compress_buffer()
 * 
 * @param in        Buffer comprimido
 * @param in_len    Longitud del buffer comprimido
 * @param out       (Salida) Buffer descomprimido (malloc)
 * @param out_len   (Salida) Tamaño del buffer descomprimido
 * @return 0 si tuvo éxito, -1 si falló
 */
int hp_decompress_buffer(const uint8_t* in, size_t in_len, uint8_t** out, size_t* out_len);

#endif
