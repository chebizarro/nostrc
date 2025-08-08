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
    return 0;
}
