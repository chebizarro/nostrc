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

#endif // GO_ERROR_H
