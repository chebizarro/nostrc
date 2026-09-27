#include <stdio.h>
#include <assert.h>
#include "int_array.h"

int main(void){
    IntArray a; int_array_init(&a);
    for (int i=0;i<10;i++) int_array_add(&a, i);
    if (int_array_size(&a) != 10) return 1;
    if (int_array_get(&a, 5) != 5) return 2;
    int_array_remove(&a, 0);
    if (int_array_get(&a, 0) != 1) return 3;
    int_array_remove(&a, int_array_size(&a)-1);
    if (int_array_size(&a) != 8) return 4;
    if (!int_array_contains(&a, 5)) return 5;
    int_array_free(&a);

    /* nostrc-6tuz: a zeroed IntArray (capacity 0, data NULL) must grow. */
    IntArray z = {0};
    for (int i = 0; i < 100; i++) int_array_add(&z, i);
    if (int_array_size(&z) != 100) return 6;
    if (int_array_get(&z, 99) != 99) return 7;
    if (z.capacity < z.size) return 8;
    /* ...and so must one reused after int_array_free() (capacity reset to 0). */
    int_array_free(&z);
    int_array_add(&z, 42);
    if (int_array_get(&z, 0) != 42) return 9;
    int_array_free(&z);
    return 0;
}
