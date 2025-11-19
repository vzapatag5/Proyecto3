#ifndef JOURNAL_H
#define JOURNAL_H

#include <stdio.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Estructura para manejar el estado del journaling */
typedef struct {
    int enabled;
    FILE* output;  /* stderr por defecto */
} Journal;

/* Inicializa el journal (por defecto deshabilitado, salida a stderr) */
void journal_init(Journal* j);

/* Habilita/deshabilita el journaling */
void journal_set_enabled(Journal* j, int enabled);

/* Cambia el stream de salida (ej: a archivo) */
void journal_set_output(Journal* j, FILE* output);

/* Imprime mensaje si está habilitado (formato printf) */
void journal_log(const Journal* j, const char* fmt, ...);

/* Macros convenientes para usar en el código */
#define JLOG(journal, ...) journal_log((journal), __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* JOURNAL_H */