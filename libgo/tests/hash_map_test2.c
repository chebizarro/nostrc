#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "hash_map.h"

static int fail(const char *msg){ fprintf(stderr, "%s\n", msg); return 1; }

int main(void){
    GoHashMap *m = go_hash_map_create(8);
    if (!m) return fail("create failed");

    // insert/get string
    go_hash_map_insert_str(m, "k", (void*)"v1");
    const char *v = (const char*)go_hash_map_get_string(m, "k");
    if (!v || strcmp(v, "v1") != 0) return fail("get v1 failed");

    // overwrite value
    go_hash_map_insert_str(m, "k", (void*)"v2");
    v = (const char*)go_hash_map_get_string(m, "k");
    if (!v || strcmp(v, "v2") != 0) return fail("overwrite failed");

    // int key
    go_hash_map_insert_int(m, 123, (void*)"iv");
    const char *iv = (const char*)go_hash_map_get_int(m, 123);
    if (!iv || strcmp(iv, "iv") != 0) return fail("int get failed");

    // remove
    go_hash_map_remove_str(m, "k");
    if (go_hash_map_get_string(m, "k") != NULL) return fail("remove str failed");
    go_hash_map_remove_int(m, 123);
    if (go_hash_map_get_int(m, 123) != NULL) return fail("remove int failed");

    go_hash_map_destroy(m);
    return 0;
}
