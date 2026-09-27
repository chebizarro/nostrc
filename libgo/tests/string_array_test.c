#include <stdio.h>
#include <string.h>
#include "string_array.h"

int main(void){
    StringArray s; string_array_init(&s);
    string_array_add(&s, "a");
    string_array_add(&s, "b");
    if (string_array_size(&s) != 2) return 1;
    if (strcmp(string_array_get(&s,0), "a") != 0) return 2;
    string_array_remove(&s, 0);
    if (strcmp(string_array_get(&s,0), "b") != 0) return 3;
    if (!string_array_contains(&s, "b")) return 4;
    string_array_free(&s);

    /* nostrc-6tuz: zeroed StringArray (capacity 0, data NULL) must grow. */
    StringArray z = {0};
    char buf[16];
    for (int i = 0; i < 50; i++) {
        snprintf(buf, sizeof buf, "s%d", i);
        string_array_add(&z, buf);
    }
    if (string_array_size(&z) != 50) return 5;
    if (strcmp(string_array_get(&z, 49), "s49") != 0) return 6;
    string_array_free(&z);

    /* The variadic helpers must keep capacity truthful so a later add
     * cannot "double" a stale capacity below size. */
    StringArray v; string_array_init(&v);
    string_array_add_many(&v, "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", NULL);
    if (v.capacity < v.size) return 7;
    string_array_add(&v, "k");
    if (string_array_size(&v) != 11) return 8;
    if (strcmp(string_array_get(&v, 10), "k") != 0) return 9;
    string_array_free(&v);

    StringArray w = {0};
    string_array_init_with(&w, "x", "y", "z", "q", "r", NULL);
    string_array_add(&w, "t");
    if (string_array_size(&w) != 6 || strcmp(string_array_get(&w, 5), "t") != 0) return 10;
    string_array_free(&w);
    return 0;
}
