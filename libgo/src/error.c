#include "error.h"
#include "go_auto.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

GO_DEFINE_AUTOPTR_CLEANUP_FUNC(Error, free_error)

// Creates a new error with a code and message
Error *new_error(int code, const char *format, ...) {
    go_autoptr(Error) err = (Error *)calloc(1, sizeof(Error));
    if (err == NULL) {
        return NULL;  // Allocation failure
    }
    
    err->code = code;
    
    // Create a formatted error message (similar to printf)
    va_list args;
    va_start(args, format);
    int len = vsnprintf(NULL, 0, format, args);
    va_end(args);
    
    if (len < 0) {
        return NULL;  // err auto-freed via go_autoptr
    }
    
    size_t message_length = (size_t)len + 1;
    err->message = (char *)malloc(message_length);
    if (err->message == NULL) {
        return NULL;  // err auto-freed via go_autoptr
    }
    
    va_start(args, format);
    vsnprintf(err->message, message_length, format, args);
    va_end(args);

    return go_steal_pointer(&err);  // Transfer ownership to caller
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
