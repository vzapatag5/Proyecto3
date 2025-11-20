// =============================================================================
// Módulo de ayuda para manejar imágenes PNG completamente en memoria
// =============================================================================
// Ofrece dos funciones principales:
//   - png_decode_image: toma los bytes de un PNG y devuelve un buffer de
//     píxeles ya expandidos a RGBA (4 canales: rojo, verde, azul, alfa).
//   - png_encode_image: toma un buffer RGB o RGBA y genera los bytes PNG.
//
// ¿Por qué convertir a RGBA al decodificar?
//   Un formato uniforme (siempre 4 canales) simplifica el uso posterior de
//   nuestros algoritmos de compresión o encriptación, evitando casos especiales
//   (grayscale, palette, transparencia separada, etc.).
//
// Cómo funciona libpng (simplificado):
//   - Creamos estructuras de lectura/escritura.
//   - Registramos funciones para leer/escribir desde memoria (no archivo).
//   - Ajustamos transformaciones (palette->RGB, añadir alfa, convertir gris a RGB).
//   - Leemos/escribimos fila por fila.
//
// Manejo de errores:
//   libpng usa longjmp para saltar en caso de error; configuramos handlers
//   silenciosos para no imprimir mensajes y solo abortar limpiamente.
// =============================================================================

#include "image_png.h"
#include <png.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

// Lectura desde memoria: estructura que guarda los bytes y posición actual.
typedef struct { const uint8_t* data; size_t size; size_t offset; } mem_reader;
// Función de callback para que libpng lea "count" bytes desde nuestro buffer.
static void png_mem_read(png_structp png_ptr, png_bytep out, png_size_t count) {
    mem_reader* r = (mem_reader*)png_get_io_ptr(png_ptr);
    if (r->offset + count > r->size) png_error(png_ptr, "Read past end"); // evita leer fuera
    memcpy(out, r->data + r->offset, count);
    r->offset += count;
}

// Escritura a memoria: vamos acumulando bytes y expandiendo el buffer con realloc.
typedef struct { uint8_t* buf; size_t size; size_t cap; } mem_writer;
static void png_mem_write(png_structp png_ptr, png_bytep data, png_size_t length) {
    mem_writer* w = (mem_writer*)png_get_io_ptr(png_ptr);
    if (w->size + length > w->cap) {
        size_t nc = (w->cap ? w->cap*2 : 4096); // crecer exponencialmente
        while (nc < w->size + length) nc *= 2;
        uint8_t* tmp = (uint8_t*)realloc(w->buf, nc);
        if (!tmp) png_error(png_ptr, "Out of memory");
        w->buf = tmp; w->cap = nc;
    }
    memcpy(w->buf + w->size, data, length);
    w->size += length;
}
static void png_mem_flush(png_structp png_ptr) { (void)png_ptr; /* sin acción */ }

/* ------------------------------------------------------------------------- */
/* Decodifica PNG y entrega píxeles en formato RGBA (4 canales).             */
/* ------------------------------------------------------------------------- */
/* Manejadores de error/advertencia silenciosos para evitar impresiones. */
static void png_silent_error(png_structp png_ptr, png_const_charp msg) {
    (void)msg;
    longjmp(png_jmpbuf(png_ptr), 1);
}
static void png_silent_warn(png_structp png_ptr, png_const_charp msg) { (void)png_ptr; (void)msg; }

int png_decode_image(const uint8_t* in_buf, size_t in_len,
                     uint8_t** out_pixels, size_t* out_len,
                     int* width, int* height, int* channels) {
    if (!in_buf || in_len < 8) return -1;
    png_structp png_ptr = NULL;
    png_infop info_ptr = NULL;
    mem_reader reader = { in_buf, in_len, 0 };
    int ret = -1;
    png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, png_silent_error, png_silent_warn); // crear estado
    if (!png_ptr) return -1;
    info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) { png_destroy_read_struct(&png_ptr, NULL, NULL); return -1; }
    if (setjmp(png_jmpbuf(png_ptr))) { goto cleanup; }

    png_set_read_fn(png_ptr, &reader, png_mem_read); // usar nuestra función de lectura
    png_read_info(png_ptr, info_ptr);               // leer cabecera y metadatos

    png_uint_32 w = 0, h = 0;
    int color_type = 0, bit_depth = 0;
    png_get_IHDR(png_ptr, info_ptr, &w, &h, &bit_depth, &color_type, NULL, NULL, NULL); // obtener ancho, alto, tipo

    // Normalizar el formato de salida antes de leer los píxeles:
    if (bit_depth == 16) png_set_strip_16(png_ptr); // reducir 16 bits a 8 bits por canal
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png_ptr); // convertir paleta a RGB
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png_ptr); // transparencia -> alfa
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png_ptr); // gris -> RGB

    png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER); // añadir canal alfa lleno (0xFF)
    png_read_update_info(png_ptr, info_ptr);        // aplicar transformaciones

    png_size_t rowbytes = png_get_rowbytes(png_ptr, info_ptr); // bytes por fila ya transformada
    size_t pixels_size = (size_t)rowbytes * h;                 // tamaño total del buffer
    uint8_t* pixels = NULL;
    png_bytep* rows = NULL;

    pixels = (uint8_t*)malloc(pixels_size ? pixels_size : 1); // asignar memoria para píxeles
    if (!pixels) goto cleanup;

    rows = (png_bytep*)malloc(sizeof(png_bytep) * h); // arreglo de punteros a cada fila
    if (!rows) { goto cleanup; }
    for (png_uint_32 y = 0; y < h; ++y) rows[y] = pixels + y * rowbytes; // apuntar cada fila dentro del buffer

    png_read_image(png_ptr, rows); // leer todas las filas
    png_read_end(png_ptr, NULL);   // finalizar lectura

    *out_pixels = pixels;    // devolver buffer de píxeles
    *out_len = pixels_size;  // longitud en bytes
    *width = (int)w;
    *height = (int)h;
    *channels = 4;           // siempre RGBA
    ret = 0;

cleanup:
    if (rows) free(rows);
    if (ret != 0) { if (pixels) free(pixels); }
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL); // liberar estructuras
    return ret;
}

/* ------------------------------------------------------------------------- */
/* Codifica un buffer RGB o RGBA en formato PNG (en memoria).                */
/* ------------------------------------------------------------------------- */
int png_encode_image(const uint8_t* pixels, size_t pixels_len,
                     int width, int height, int channels,
                     uint8_t** out_buf, size_t* out_buf_len) {
    if (!pixels || width <= 0 || height <= 0 || (channels != 3 && channels != 4)) return -1;

    png_structp png_ptr = NULL;
    png_infop info_ptr = NULL;
    mem_writer writer = { NULL, 0, 0 };
    int ret = -1;

    png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL); // crear estado de escritura
    if (!png_ptr) return -1;
    info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) { png_destroy_write_struct(&png_ptr, NULL); return -1; }
    if (setjmp(png_jmpbuf(png_ptr))) { goto cleanup; }

    png_set_write_fn(png_ptr, &writer, png_mem_write, png_mem_flush); // usar escritura a memoria

    int color_type = (channels == 4) ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB; // decidir tipo
    png_set_IHDR(png_ptr, info_ptr, (png_uint_32)width, (png_uint_32)height,
                 8, color_type, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT); // cabecera
    png_write_info(png_ptr, info_ptr); // escribir información inicial

    png_bytep* rows = (png_bytep*)malloc(sizeof(png_bytep) * height); // reservar array de filas
    if (!rows) goto cleanup;
    png_size_t rowbytes = (png_size_t)width * channels; // bytes por fila
    for (int y = 0; y < height; ++y) rows[y] = (png_bytep)(pixels + (size_t)y * rowbytes); // mapear filas

    png_write_image(png_ptr, rows); // escribir todas las filas
    png_write_end(png_ptr, info_ptr); // finalizar escritura

    *out_buf = writer.buf;      // devolver buffer PNG
    *out_buf_len = writer.size; // tamaño total
    writer.buf = NULL;          // transferencia de propiedad
    ret = 0;

    free(rows);
cleanup:
    if (writer.buf) free(writer.buf);
    png_destroy_write_struct(&png_ptr, &info_ptr); // liberar estructuras
    return ret;
}