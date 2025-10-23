#include "fs.h"
#include <fcntl.h>     // open
#include <unistd.h>    // read, write, close
#include <sys/stat.h>  // stat
#include <stdlib.h>    // malloc, free

int read_file(const char* path, uint8_t** out_buf, size_t* out_len) {
    *out_buf = NULL; *out_len = 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    size_t cap = 1024;
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (!buf) { close(fd); return -1; }

    size_t total = 0;
    for (;;) {
        if (total == cap) {
            size_t ncap = cap * 2;
            uint8_t* tmp = (uint8_t*)realloc(buf, ncap);
            if (!tmp) { free(buf); close(fd); return -1; }
            buf = tmp; cap = ncap;
        }
        ssize_t n = read(fd, buf + total, cap - total);
        if (n < 0)  { free(buf); close(fd); return -1; }
        if (n == 0) break;
        total += (size_t)n;
    }
    close(fd);

    *out_buf = buf;
    *out_len = total;
    return 0;
}

int write_file(const char* path, const uint8_t* buf, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    size_t total = 0;
    while (total < len) {
        ssize_t n = write(fd, buf + total, len - total);
        if (n <= 0) { close(fd); return -1; }
        total += (size_t)n;
    }
    close(fd);
    return 0;
}
