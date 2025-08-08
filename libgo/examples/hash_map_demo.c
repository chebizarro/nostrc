#include <stdio.h>
#include <stdbool.h>
#include "hash_map.h"

static bool print_kv(HashKey *k, void *v){
    // We don't know HashKey internals here; this demo just prints the value pointer.
    printf("value=%p\n", v);
    return true; // continue
}

int main(){
    GoHashMap *m = go_hash_map_create(64);

    go_hash_map_insert_str(m, "name", (void*)"alice");
    go_hash_map_insert_int(m, 123, (void*)"id123");

    const char *name = (const char*)go_hash_map_get_string(m, "name");
    const char *idv = (const char*)go_hash_map_get_int(m, 123);
    printf("name=%s id=%s\n", name, idv);

    go_hash_map_for_each(m, print_kv);

    go_hash_map_remove_str(m, "name");
    go_hash_map_remove_int(m, 123);

    go_hash_map_destroy(m);
    return 0;
}
