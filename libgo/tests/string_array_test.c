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
    return 0;
}
