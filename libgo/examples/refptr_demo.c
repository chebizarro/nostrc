#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "refptr.h"

static void free_buf(void *p){
    printf("freeing buffer\n");
    free(p);
}

int main(){
    // Create a ref-counted buffer
    go_autoptr(Buffer) r = make_go_refptr(malloc(128), free_buf);
    strcpy((char*)r.ptr, "hello refptr");
    printf("%s\n", (char*)r.ptr);

    // Retain/release
    go_refptr_retain(&r);
    go_refptr_release(&r);

    // go_autostr demo
    go_autostr dyn = strdup("auto-free string");
    printf("%s\n", dyn);

    // r and dyn auto-clean at end of scope
    return 0;
}
