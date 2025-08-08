#ifndef GO_ERROR_H
#define GO_ERROR_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Define the Error struct
typedef struct _Error {
    int code;            // Error code
    char *message;       // Error message
} Error;

Error *new_error(int code, const char *format, ...);

void free_error(Error *err);

void print_error(const Error *err);

int is_error(const Error *err);

#endif // GO_ERROR_H
