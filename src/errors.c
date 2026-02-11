#include "errors.h"
#include <stdio.h>
#include <stdarg.h>

void exodus_error(const char* format, ...) {
    va_list args;
    va_start(args, format);
    
    // Red color for [Exodus]: Error message
    fprintf(stderr, "[Exodus]: ");
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    
    va_end(args);
}

void exodus_print(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}
