#ifndef FS_H
#define FS_H
#include <stddef.h>
#include <stdint.h>

int read_file(const char* path, uint8_t** out_buf, size_t* out_len);
int write_file(const char* path, const uint8_t* buf, size_t len);

#endif
