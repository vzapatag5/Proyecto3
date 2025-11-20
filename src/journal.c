// ============================================================================
// journal.c - Pequeño sistema de "journal" (registro/log) opcional
// ----------------------------------------------------------------------------
// Este módulo permite imprimir mensajes de seguimiento (debug / progreso)
// solo cuando está habilitado. Así evitamos llenar la salida en usos normales.
//
// Idea básica:
//   - Crear/ inicializar la estructura Journal.
//   - Activar o desactivar con journal_set_enabled().
//   - Usar journal_log() para escribir mensajes formateados (similar a printf).
//   - Se fuerza un fflush() para que el mensaje aparezca de inmediato.
//
// Notas:
//   - Si no está habilitado (enabled = 0) el costo de llamar journal_log es mínimo.
//   - Se puede redirigir la salida a un archivo usando journal_set_output().
//   - Usa varargs ("...") para aceptar formato variable como printf.
// ============================================================================

#include "journal.h"
#include <stdarg.h>

// Inicializa la estructura: por defecto deshabilitado y salida estándar de errores.
void journal_init(Journal* j) {
    if (!j) return;
    j->enabled = 0;      // comienza apagado
    j->output = stderr;  // se puede cambiar luego
}

// Activa (enabled=1) o desactiva (enabled=0) el journal.
void journal_set_enabled(Journal* j, int enabled) {
    if (!j) return;
    j->enabled = enabled ? 1 : 0;
}

// Cambia el destino donde se escriben los mensajes (ej: archivo).
void journal_set_output(Journal* j, FILE* output) {
    if (!j || !output) return;
    j->output = output;
}

// Imprime un mensaje formateado si el journal está habilitado.
// Uso típico: journal_log(&j, "Procesando chunk %zu/%zu\n", i, total);
// Se ejecuta fflush para ver el mensaje al instante (útil en largas operaciones).
void journal_log(const Journal* j, const char* fmt, ...) {
    if (!j || !j->enabled || !j->output || !fmt) return; // si está apagado, salir rápido

    va_list args;
    va_start(args, fmt);
    vfprintf(j->output, fmt, args); // imprime con formato variable
    va_end(args);

    fflush(j->output);  // asegurar escritura inmediata
}