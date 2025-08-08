#include "error.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

// Creates a new error with a code and message
Error *new_error(int code, const char *format, ...) {
    Error *err = (Error *)malloc(sizeof(Error));
    if (err == NULL) {
        return NULL;  // Allocation failure
    }
    
    err->code = code;
    
    // Create a formatted error message (similar to printf)
    va_list args;
    va_start(args, format);
    size_t message_length = vsnprintf(NULL, 0, format, args) + 1;
    va_end(args);
    
    err->message = (char *)malloc(message_length);
    if (err->message == NULL) {
        free(err);
        return NULL;  // Allocation failure
    }
    
    va_start(args, format);
    vsnprintf(err->message, message_length, format, args);
    va_end(args);

    return err;
}

// Frees the memory allocated for the error
void free_error(Error *err) {
    if (err != NULL) {
        if (err->message != NULL) {
            free(err->message);
        }
        free(err);
    }
}

// Prints the error message to stderr
void print_error(const Error *err) {
    if (err != NULL) {
        fprintf(stderr, "Error [%d]: %s\n", err->code, err->message);
    }
}

// Check if there is an error
int is_error(const Error *err) {
    return err != NULL;
}
