#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "json.h"

static void test_top_level_ints(void) {
    const char *js = "{\"nums\":[1,2,3]}";
    int *arr = NULL; size_t n = 0;
    int rc = nostr_json_get_int_array(js, "nums", &arr, &n);
    assert(rc == 0);
    assert(arr != NULL);
    assert(n == 3);
    assert(arr[0] == 1 && arr[1] == 2 && arr[2] == 3);
    free(arr);
}

static void test_top_level_reals_truncate(void) {
    const char *js = "{\"nums\":[1.2,3.9,-2.1]}";
    int *arr = NULL; size_t n = 0;
    int rc = nostr_json_get_int_array(js, "nums", &arr, &n);
    assert(rc == 0);
    assert(arr != NULL);
    assert(n == 3);
    assert(arr[0] == (int)1.2);
    assert(arr[1] == (int)3.9);
    assert(arr[2] == (int)-2.1);
    free(arr);
}

static void test_top_level_empty(void) {
    const char *js = "{\"nums\":[]}";
    int *arr = NULL; size_t n = 0;
    int rc = nostr_json_get_int_array(js, "nums", &arr, &n);
    assert(rc == 0);
    assert(arr != NULL);
    assert(n == 0);
    free(arr);
}

static void test_top_level_non_numeric_fails(void) {
    const char *js = "{\"nums\":[1,\"x\"]}";
    int *arr = NULL; size_t n = 0;
    int rc = nostr_json_get_int_array(js, "nums", &arr, &n);
    assert(rc == -1);
    assert(arr == NULL);
    assert(n == 0);
}

static void test_top_level_not_array_fails(void) {
    const char *js = "{\"nums\":\"nope\"}";
    int *arr = NULL; size_t n = 0;
    int rc = nostr_json_get_int_array(js, "nums", &arr, &n);
    assert(rc == -1);
    assert(arr == NULL);
    assert(n == 0);
}

static void test_nested_ints(void) {
    const char *js = "{\"obj\":{\"nums\":[10,20]}}";
    int *arr = NULL; size_t n = 0;
    int rc = nostr_json_get_int_array_at(js, "obj", "nums", &arr, &n);
    assert(rc == 0);
    assert(arr != NULL);
    assert(n == 2);
    assert(arr[0] == 10 && arr[1] == 20);
    free(arr);
}

static void test_nested_reals_truncate(void) {
    const char *js = "{\"obj\":{\"nums\":[10.9,-0.2]}}";
    int *arr = NULL; size_t n = 0;
    int rc = nostr_json_get_int_array_at(js, "obj", "nums", &arr, &n);
    assert(rc == 0);
    assert(arr != NULL);
    assert(n == 2);
    assert(arr[0] == (int)10.9);
    assert(arr[1] == (int)-0.2);
    free(arr);
}

static void test_nested_empty(void) {
    const char *js = "{\"obj\":{\"nums\":[]}}";
    int *arr = NULL; size_t n = 0;
    int rc = nostr_json_get_int_array_at(js, "obj", "nums", &arr, &n);
    assert(rc == 0);
    assert(arr != NULL);
    assert(n == 0);
    free(arr);
}

static void test_nested_non_numeric_fails(void) {
    const char *js = "{\"obj\":{\"nums\":[0,\"bad\"]}}";
    int *arr = NULL; size_t n = 0;
    int rc = nostr_json_get_int_array_at(js, "obj", "nums", &arr, &n);
    assert(rc == -1);
    assert(arr == NULL);
    assert(n == 0);
}

int main(void) {
    test_top_level_ints();
    test_top_level_reals_truncate();
    test_top_level_empty();
    test_top_level_non_numeric_fails();
    test_top_level_not_array_fails();
    test_nested_ints();
    test_nested_reals_truncate();
    test_nested_empty();
    test_nested_non_numeric_fails();
    printf("test_json_int_array: OK\n");
    return 0;
}
