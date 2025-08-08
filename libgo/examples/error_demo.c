#include <stdio.h>
#include "error.h"

int main(){
    const char *path = "/nonexistent";
    Error *e = new_error(42, "failed to open: %s", path);
    if (is_error(e)) {
        print_error(e);
    }
    free_error(e);
    return 0;
}
