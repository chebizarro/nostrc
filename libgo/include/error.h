#ifndef GO_ERROR_H
#define GO_ERROR_H

#include "libgo-export.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Define the Error struct
typedef struct _Error {
    int code;            // Error code
    char *message;       // Error message
} Error;

LIBGO_API Error *new_error(int code, const char *format, ...);

LIBGO_API void free_error(Error *err);

LIBGO_API void print_error(const Error *err);

LIBGO_API int is_error(const Error *err);

#endif // GO_ERROR_H
