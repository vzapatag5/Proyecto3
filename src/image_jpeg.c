// =============================================================================
// Módulo de ayuda para manejar imágenes JPEG en memoria
// =============================================================================
// Proporciona dos funciones sencillas:
//  - jpeg_decode_image: recibe un JPEG (bytes en memoria) y produce un buffer
//    RGB (3 canales: rojo, verde, azul) sin compresión.
//  - jpeg_encode_image: recibe un buffer RGB y lo convierte a JPEG comprimido
//    (calidad fija 90).
//
// ¿Por qué convertir a RGB sin compresión?
//  - Facilita aplicar nuestros algoritmos propios de compresión/encriptación
//    sobre datos “crudos” y no sobre el formato JPEG ya comprimido.
//
// Notas simplificadas sobre JPEG:
//  - JPEG almacena la imagen comprimida con técnicas como DCT y cuantización.
//  - La librería libjpeg se encarga de todos esos detalles internos.
//  - Aquí solo pedimos: “dame los píxeles en RGB” o “guarda este RGB como JPEG”.
//
// Importante:
//  - No manejamos canal alfa (transparencia); solo imágenes RGB estándar.
//  - La calidad de salida (encode) está fija en 90 (buena calidad). Se podría
//    hacer parámetro si se necesita.
// =============================================================================

#include "image_jpeg.h"
#include <stdio.h>
#include <jpeglib.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* Decodifica un JPEG en memoria y devuelve pixeles RGB (3 canales).          */
/* -------------------------------------------------------------------------- */
int jpeg_decode_image(const uint8_t* in_buf, size_t in_len,
                      uint8_t** out_pixels, size_t* out_len,
                      int* width, int* height, int* channels) {
    if (!in_buf || in_len < 4) return -1;
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPARRAY buffer; int row_stride;
    int ret = -1;

    cinfo.err = jpeg_std_error(&jerr);        // Inicializar manejo básico de errores
    jpeg_create_decompress(&cinfo);           // Crear objeto de descompresión
    jpeg_mem_src(&cinfo, (unsigned char*)in_buf, (unsigned long)in_len); // Fuente: memoria
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) goto cleanup;  // Leer cabecera JPEG
    cinfo.out_color_space = JCS_RGB;           // Pedimos salida en formato RGB
    jpeg_start_decompress(&cinfo);             // Iniciar proceso de decodificación

    row_stride = cinfo.output_width * cinfo.output_components; // bytes por fila
    size_t pixels_size = (size_t)row_stride * cinfo.output_height; // total de bytes
    uint8_t* pixels = (uint8_t*)malloc(pixels_size ? pixels_size : 1);
    if (!pixels) goto cleanup; // sin memoria

    // Reservar buffer temporal para una sola fila (scanline)
    buffer = (*cinfo.mem->alloc_sarray)
        ((j_common_ptr)&cinfo, JPOOL_IMAGE, (size_t)row_stride, 1);

    // Leer fila por fila y copiar al buffer final
    size_t offset = 0;
    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, buffer, 1);              // Decodifica una fila
        memcpy(pixels + offset, buffer[0], row_stride);      // Copiamos al destino
        offset += row_stride;
    }

    jpeg_finish_decompress(&cinfo); // Finalizar proceso (libjpeg limpia estados internos)

    // Pasar resultados al llamador
    *out_pixels = pixels;
    *out_len = pixels_size;
    *width = (int)cinfo.output_width;
    *height = (int)cinfo.output_height;
    *channels = cinfo.output_components; // debería ser 3 (RGB)
    ret = 0;

cleanup:
    jpeg_destroy_decompress(&cinfo); // Liberar estructuras internas
    return ret;
}

/* -------------------------------------------------------------------------- */
/* Codifica un buffer RGB a JPEG en memoria (sin alfa).                       */
/* -------------------------------------------------------------------------- */
int jpeg_encode_image(const uint8_t* pixels, size_t pixels_len,
                      int width, int height, int channels,
                      uint8_t** out_buf, size_t* out_buf_len) {
    if (!pixels || width <= 0 || height <= 0 || channels != 3) return -1;
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    unsigned char* mem = NULL; unsigned long memsize = 0;
    JSAMPROW row_pointer[1];
    int quality = 90;

    cinfo.err = jpeg_std_error(&jerr);   // Manejo básico de errores
    jpeg_create_compress(&cinfo);        // Crear objeto de compresión
    jpeg_mem_dest(&cinfo, &mem, &memsize); // Salida: escribir en memoria dinámica

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = channels; // debe ser 3
    cinfo.in_color_space = JCS_RGB;    // indicamos que la entrada es RGB

    jpeg_set_defaults(&cinfo);               // Configuración base
    jpeg_set_quality(&cinfo, quality, TRUE); // Calidad (0-100). 90 es buena
    jpeg_start_compress(&cinfo, TRUE);       // Iniciar proceso de compresión

    int row_stride = width * channels; // bytes por fila
    while (cinfo.next_scanline < cinfo.image_height) {
        row_pointer[0] = (JSAMPROW)(pixels + cinfo.next_scanline * row_stride); // puntero a la fila
        jpeg_write_scanlines(&cinfo, row_pointer, 1); // escribir una fila al JPEG
    }
    jpeg_finish_compress(&cinfo); // Finalizar (libjpeg produce bytes en 'mem')

    *out_buf = (uint8_t*)mem;      // Transferimos el buffer al llamador
    *out_buf_len = (size_t)memsize; // tamaño total del JPEG
    jpeg_destroy_compress(&cinfo);  // Liberar estructuras internas
    return 0;
}