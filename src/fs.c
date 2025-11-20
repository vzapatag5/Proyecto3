#include "fs.h"
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

// =============================================================================
// Funciones para leer y escribir archivos completos en memoria
// =============================================================================
// Este módulo proporciona operaciones básicas de entrada/salida para:
// - Leer un archivo completo y cargarlo en memoria (para comprimir/encriptar)
// - Escribir un buffer completo a disco (para guardar el resultado)

// -----------------------------------------------------------------------------
// read_file: Lee TODO el contenido de un archivo y lo devuelve en memoria
// -----------------------------------------------------------------------------
// Parámetros:
//   - path: ruta del archivo a leer
//   - out_buf: puntero donde se guardará la dirección del buffer con los datos
//   - out_len: puntero donde se guardará el tamaño de los datos leídos
// 
// Retorna: 0 si tuvo éxito, -1 si hubo algún error
//
// Nota: El buffer devuelto está en memoria dinámica (malloc), por lo que el
//       llamador debe liberar la memoria con free() cuando ya no la necesite.
int read_file(const char* path, uint8_t** out_buf, size_t* out_len) {
    // Validar que los parámetros no sean NULL
    if (!path || !out_buf || !out_len) return -1;
    *out_buf = NULL; *out_len = 0;

    // Abrir el archivo en modo solo lectura
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;  // Error al abrir (archivo no existe, sin permisos, etc.)

    // Obtener información del archivo (principalmente su tamaño)
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return -1; }
    
    // Verificar que sea un archivo regular (no un directorio o dispositivo)
    if (!S_ISREG(st.st_mode)) { close(fd); errno = EISDIR; return -1; }

    // Reservar memoria para almacenar todo el archivo
    size_t n = (size_t)st.st_size;
    uint8_t* buf = (uint8_t*)malloc(n ? n : 1);  // Si el archivo está vacío, asignar 1 byte mínimo
    if (!buf) { close(fd); return -1; }  // Sin memoria disponible

    // Leer el archivo en bloques hasta obtener todo el contenido
    // (read() puede leer menos bytes de los solicitados, por eso usamos un bucle)
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, buf + off, n - off);
        if (r < 0) { free(buf); close(fd); return -1; }  // Error de lectura
        if (r == 0) break;  // Fin del archivo
        off += (size_t)r;   // Avanzar la posición en el buffer
    }
    close(fd);

    // Devolver el buffer con los datos y su tamaño
    *out_buf = buf;
    *out_len = off;
    return 0;
}

// -----------------------------------------------------------------------------
// write_file: Escribe TODO el contenido de un buffer en un archivo
// -----------------------------------------------------------------------------
// Parámetros:
//   - path: ruta del archivo a crear/sobrescribir
//   - buf: buffer con los datos a escribir
//   - len: cantidad de bytes a escribir
//
// Retorna: 0 si tuvo éxito, -1 si hubo algún error
//
// Nota: Si el archivo ya existe, se sobrescribe completamente (se trunca).
//       Si no existe, se crea con permisos 0644 (lectura/escritura para el dueño,
//       solo lectura para grupo y otros).
int write_file(const char* path, const uint8_t* buf, size_t len) {
    // Validar parámetros
    if (!path || (!buf && len>0)) return -1;

    // Crear o abrir el archivo en modo escritura
    // O_WRONLY = solo escritura
    // O_CREAT  = crear si no existe
    // O_TRUNC  = vaciar el archivo si ya existe
    // 0644     = permisos: rw-r--r--
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;  // Error al crear/abrir

    // Escribir todos los datos del buffer al archivo
    // (write() puede escribir menos bytes de los solicitados, por eso usamos un bucle)
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w <= 0) { close(fd); return -1; }  // Error de escritura
        off += (size_t)w;  // Avanzar la posición en el buffer
    }
    
    // Opcional: forzar que los datos se escriban físicamente al disco
    // fsync(fd);  // Descomentado por defecto para mayor velocidad
    
    // Cerrar el archivo
    if (close(fd) != 0) return -1;
    return 0;
}
