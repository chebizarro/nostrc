#include "utils_extra.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

// Test function for named lock
void critical_section(void *arg) {
    int *counter = (int *)arg;
    (*counter)++;
}

void test_named_lock() {
    int counter = 0;
    named_lock("test", critical_section, &counter);
    assert(counter == 1);
}

void test_similar() {
    int as[] = {1, 2, 3};
    int bs[] = {3, 2, 1};
    assert(similar(as, 3, bs, 3) == true);

    int cs[] = {1, 2, 4};
    assert(similar(as, 3, cs, 3) == false);
}

void test_escape_string() {
    char *escaped = escape_string("Hello \"world\"\n");
    assert(strcmp(escaped, "\"Hello \\\"world\\\"\\n\"") == 0);
    free(escaped);
}

void test_are_pointer_values_equal() {
    int a = 5;
    int b = 5;
    int c = 6;
    assert(are_pointer_values_equal(&a, &b, sizeof(int)) == true);
    assert(are_pointer_values_equal(&a, &c, sizeof(int)) == false);
}

int main() {
    test_named_lock();
    test_similar();
    test_escape_string();
    test_are_pointer_values_equal();
    printf("All tests passed!\n");
    return 0;
}