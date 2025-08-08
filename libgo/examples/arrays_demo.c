#include <stdio.h>
#include "int_array.h"
#include "string_array.h"

int main(){
    IntArray ia; int_array_init(&ia);
    int_array_add(&ia, 7);
    int_array_add(&ia, 9);
    printf("ia[0]=%d size=%zu\n", int_array_get(&ia,0), int_array_size(&ia));
    int_array_remove(&ia, 0);
    printf("ia[0]=%d size=%zu\n", int_array_get(&ia,0), int_array_size(&ia));
    int_array_free(&ia);

    StringArray sa; string_array_init(&sa);
    string_array_add(&sa, "alpha");
    string_array_add(&sa, "beta");
    printf("sa[1]=%s size=%zu\n", string_array_get(&sa,1), string_array_size(&sa));
    string_array_remove(&sa, 0);
    printf("sa[0]=%s size=%zu\n", string_array_get(&sa,0), string_array_size(&sa));
    string_array_free(&sa);
    return 0;
}
