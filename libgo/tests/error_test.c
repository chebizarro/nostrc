#include <stdio.h>
#include <string.h>
#include "error.h"

int main(void){
    Error *e1 = new_error(1, "simple error");
    if (!e1 || !is_error(e1) || !e1->message) return 1;
    print_error(e1);
    free_error(e1);

    const char *path = "/very/long/path/to/file";
    Error *e2 = new_error(42, "failed to open: %s (%d)", path, 7);
    if (!e2 || !is_error(e2) || strstr(e2->message, path) == NULL) return 2;
    free_error(e2);
    return 0;
}
