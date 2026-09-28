#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "json.h"
#include "nostr_jansson.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static size_t backend_calls;

static int counting_deserialize(NostrFilter *filter, const char *json) {
    backend_calls++;
    return jansson_impl->deserialize_filter(filter, json);
}

static int run_seed(const char *path, int expect_fallback) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        perror(path);
        return 1;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 1;
    }
    long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 1;
    }
    uint8_t *data = malloc((size_t)length);
    if (!data) {
        fclose(file);
        return 1;
    }
    size_t read_size = fread(data, 1, (size_t)length, file);
    fclose(file);
    if (read_size != (size_t)length) {
        free(data);
        return 1;
    }

    size_t before = backend_calls;
    LLVMFuzzerTestOneInput(data, read_size);
    free(data);
    if (backend_calls - before != (size_t)expect_fallback) {
        fprintf(stderr, "%s: expected %d Jansson calls, got %zu\n",
                path, expect_fallback, backend_calls - before);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 6) {
        fprintf(stderr, "expected two compact and three fallback seed paths\n");
        return 1;
    }
    const uint8_t bootstrap[] = "{}";
    LLVMFuzzerTestOneInput(bootstrap, sizeof(bootstrap) - 1);
    if (json_interface != jansson_impl || !jansson_impl->deserialize_filter) {
        fprintf(stderr, "fuzz harness did not install Jansson\n");
        return 1;
    }
    NostrJsonInterface probe = *jansson_impl;
    probe.deserialize_filter = counting_deserialize;
    nostr_set_json_interface(&probe);

    return run_seed(argv[1], 0) || run_seed(argv[2], 1) ||
           run_seed(argv[3], 0) || run_seed(argv[4], 1) ||
           run_seed(argv[5], 1);
}
