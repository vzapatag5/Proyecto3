#ifndef FS_H
#define FS_H
#include <stddef.h>
#include <stdint.h>

/* Lee TODO el archivo en memoria (buffer malloc). Devuelve 0 si ok. */
int read_file(const char* path, uint8_t** out_buf, size_t* out_len);

/* Escribe TODO el buffer en path (crea/trunca). Devuelve 0 si ok. */
int write_file(const char* path, const uint8_t* buf, size_t len);

#endif
