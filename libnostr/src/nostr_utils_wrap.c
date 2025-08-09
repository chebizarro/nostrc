#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "utils.h"

uint64_t nostr_memhash(const char *data, size_t len) {
    return memhash(data, len);
}

void nostr_named_lock(const char *name, void (*critical_section)(void *), void *arg) {
    named_lock(name, critical_section, arg);
}

bool nostr_similar(const int *a, size_t a_len, const int *b, size_t b_len) {
    return similar(a, a_len, b, b_len);
}

char *nostr_escape_string(const char *s) {
    return escape_string(s);
}

bool nostr_pointer_values_equal(const void *a, const void *b, size_t size) {
    return are_pointer_values_equal(a, b, size);
}

char *nostr_normalize_url(const char *in) {
    return normalize_url(in);
}

char *nostr_normalize_ok_message(const char *reason, const char *prefix) {
    return normalize_ok_message(reason, prefix);
}

bool nostr_hex2bin(unsigned char *bin, const char *hex, size_t bin_len) {
    return hex2bin(bin, hex, bin_len);
}

int64_t nostr_sub_id_to_serial(const char *sub_id) {
    return sub_id_to_serial(sub_id);
}
