#include <stdio.h>
#include "nip54.h"

int main() {
    const char *name = "  ExaMPLE Identifier 123  ";
    char *normalized = normalize_identifier(name);
    if (normalized) {
        printf("Normalized Identifier: %s\n", normalized);
        free(normalized);
    } else {
        printf("Failed to normalize identifier\n");
    }
    return 0;
}
