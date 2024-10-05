#include "utils.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void test_normalize_url() {
    char *result;

    result = normalize_url("http://example.com");
    assert(strcmp(result, "ws://example.com") == 0);
    free(result);

    result = normalize_url("https://example.com");
    assert(strcmp(result, "wss://example.com") == 0);
    free(result);

    result = normalize_url("ws://example.com");
    assert(strcmp(result, "ws://example.com") == 0);
    free(result);

    result = normalize_url("wss://example.com");
    assert(strcmp(result, "wss://example.com") == 0);
    free(result);

    result = normalize_url("localhost:8080");
    assert(strcmp(result, "ws://localhost:8080") == 0);
    free(result);

    result = normalize_url("example.com");
    assert(strcmp(result, "wss://example.com") == 0);
    free(result);

    result = normalize_url("  HTTP://EXAMPLE.COM/PATH/ ");
    assert(strcmp(result, "ws://example.com/path") == 0);
    free(result);
}

void test_normalize_ok_message() {
    char *result;

    result = normalize_ok_message("reason", "prefix");
    assert(strcmp(result, "prefix: reason") == 0);
    free(result);

    result = normalize_ok_message("auth-required: token", "prefix");
    assert(strcmp(result, "auth-required: token") == 0);
    free(result);

    result = normalize_ok_message("bad prefix: token", "auth");
    assert(strcmp(result, "auth: bad prefix: token") == 0);
    free(result);

    result = normalize_ok_message("bad:prefix token", "auth");
    assert(strcmp(result, "auth: bad:prefix token") == 0);
    free(result);
}

int main() {
    test_normalize_url();
    test_normalize_ok_message();
    printf("All tests passed!\n");
    return 0;
}