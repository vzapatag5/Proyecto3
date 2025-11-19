#include "journal.h"
#include <stdarg.h>

void journal_init(Journal* j) {
    if (!j) return;
    j->enabled = 0;
    j->output = stderr;
}

void journal_set_enabled(Journal* j, int enabled) {
    if (!j) return;
    j->enabled = enabled ? 1 : 0;
}

void journal_set_output(Journal* j, FILE* output) {
    if (!j || !output) return;
    j->output = output;
}

void journal_log(const Journal* j, const char* fmt, ...) {
    if (!j || !j->enabled || !j->output || !fmt) return;
    
    va_list args;
    va_start(args, fmt);
    vfprintf(j->output, fmt, args);
    va_end(args);
    
    fflush(j->output);  /* Asegurar que se escriba inmediatamente */
}