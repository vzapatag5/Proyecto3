#include "fs.h"
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

int read_file(const char* path, uint8_t** out_buf, size_t* out_len) {
    if (!path || !out_buf || !out_len) return -1;
    *out_buf = NULL; *out_len = 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return -1; }
    if (!S_ISREG(st.st_mode)) { close(fd); errno = EISDIR; return -1; }

    size_t n = (size_t)st.st_size;
    uint8_t* buf = (uint8_t*)malloc(n ? n : 1);
    if (!buf) { close(fd); return -1; }

    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, buf + off, n - off);
        if (r < 0) { free(buf); close(fd); return -1; }
        if (r == 0) break;
        off += (size_t)r;
    }
    close(fd);

    *out_buf = buf;
    *out_len = off;
    return 0;
}

int write_file(const char* path, const uint8_t* buf, size_t len) {
    if (!path || (!buf && len>0)) return -1;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w <= 0) { close(fd); return -1; }
        off += (size_t)w;
    }
    // fsync opcional (según requisitos de durabilidad)
    // fsync(fd);
    if (close(fd) != 0) return -1;
    return 0;
}
