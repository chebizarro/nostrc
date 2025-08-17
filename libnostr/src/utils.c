#include "nostr-utils.h"
#include <ctype.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <nsync.h>

// Helper function to trim leading and trailing whitespace
static char *nostr_trim_whitespace(char *str) {
    char *end;

    // Trim leading space
    while (isspace((unsigned char)*str))
        str++;

    if (*str == 0) {
        return str;
    }


    // Trim trailing space
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end))
        end--;

    end[1] = '\0';

    return str;
}

// NormalizeURL normalizes the url and replaces http://, https:// schemes with ws://, wss://
char *nostr_normalize_url(const char *u) {
    if (!u || strlen(u) == 0) {
        return strdup("");
    }

    char *url = strdup(u);
    if (!url)
        return NULL;

    url = nostr_trim_whitespace(url);

    for (char *p = url; *p; ++p) {
        *p = tolower((unsigned char)*p);
    }

    if (strstr(url, "localhost") == url) {
        char *temp = (char *)malloc(strlen("ws://") + strlen(url) + 1);
        if (!temp) {
            free(url);
            return NULL;
        }
        strcpy(temp, "ws://");
        strcat(temp, url);
        free(url);
        url = temp;
    } else if (strncmp(url, "http", 4) != 0 && strncmp(url, "ws", 2) != 0) {
        char *temp = (char *)malloc(strlen("wss://") + strlen(url) + 1);
        if (!temp) {
            free(url);
            return NULL;
        }
        strcpy(temp, "wss://");
        strcat(temp, url);
        free(url);
        url = temp;
    }

    char *scheme_end = strstr(url, "://");
    if (scheme_end) {
        char *scheme = strndup(url, scheme_end - url);
        if (strcmp(scheme, "http") == 0) {
            strcpy(scheme, "ws");
        } else if (strcmp(scheme, "https") == 0) {
            strcpy(scheme, "wss");
        }
        strncpy(url, scheme, strlen(scheme));
        free(scheme);
    }

    // Normalize path
    char *path = strchr(url, '/');
    if (path) {
        char *normalized_path = strdup(path);
        if (!normalized_path) {
            free(url);
            return NULL;
        }
        char *end = normalized_path + strlen(normalized_path) - 1;
        while (end > normalized_path && *end == '/') {
            *end = '\0';
            end--;
        }
        strcpy(path, normalized_path);
        free(normalized_path);
    }

    return url;
}

// NormalizeOKMessage takes a string message that is to be sent in an `OK` or `CLOSED` command
char *nostr_normalize_ok_message(const char *reason, const char *prefix) {
    if (!reason || strlen(reason) == 0 || !prefix) {
        return strdup(prefix);
    }

    const char *delimiter = ": ";
    char *colon_pos = strstr(reason, delimiter);
    if (!colon_pos || strchr(reason, ' ') < colon_pos) {
        size_t prefix_len = strlen(prefix);
        size_t reason_len = strlen(reason);
        size_t total_len = prefix_len + strlen(delimiter) + reason_len + 1;

        char *result = (char *)malloc(total_len);
        if (!result) {
            return NULL;
        }

        strcpy(result, prefix);
        strcat(result, delimiter);
        strcat(result, reason);

        return result;
    }

    return strdup(reason);
}

// Initialize mutex pool
pthread_mutex_t nostr_named_mutex_pool[MAX_LOCKS] = {PTHREAD_MUTEX_INITIALIZER};

// Simple hash function for demonstration
uint64_t nostr_memhash(const char *data, size_t len) {
    uint64_t hash = 5381;
    for (size_t i = 0; i < len; i++) {
        hash = ((hash << 5) + hash) + data[i];
    }
    return hash;
}

// Function to acquire a named lock
void nostr_named_lock(const char *name, void (*critical_section)(void *), void *arg) {
    uint64_t hash = nostr_memhash(name, strlen(name));
    size_t idx = hash % MAX_LOCKS;
    pthread_mutex_lock(&nostr_named_mutex_pool[idx]);
    critical_section(arg);
    pthread_mutex_unlock(&nostr_named_mutex_pool[idx]);
}

// Function to compare two arrays for similarity
bool nostr_similar(const int *as, size_t as_len, const int *bs, size_t bs_len) {
    if (as_len != bs_len) {
        return false;
    }

    for (size_t i = 0; i < as_len; i++) {
        bool found = false;
        for (size_t j = 0; j < bs_len; j++) {
            if (as[i] == bs[j]) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }

    return true;
}


// Function to escape a string for JSON encoding (without surrounding quotes)
char *nostr_escape_string(const char *s) {
    if (!s) return strdup("");
    size_t len = strlen(s);
    // Rough estimate: worst case every char needs escaping (e.g., \u00XX) => up to 6x
    size_t cap = len * 6 + 1;
    char *escaped = (char *)malloc(cap);
    if (!escaped) return NULL;

    char *dst = escaped;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':
            *dst++ = '\\'; *dst++ = '"';
            break;
        case '\\':
            *dst++ = '\\'; *dst++ = '\\';
            break;
        case '\b':
            *dst++ = '\\'; *dst++ = 'b';
            break;
        case '\t':
            *dst++ = '\\'; *dst++ = 't';
            break;
        case '\n':
            *dst++ = '\\'; *dst++ = 'n';
            break;
        case '\f':
            *dst++ = '\\'; *dst++ = 'f';
            break;
        case '\r':
            *dst++ = '\\'; *dst++ = 'r';
            break;
        default:
            if (iscntrl(c)) {
                // ensure capacity (up to 6 bytes: \uXXXX)
                int n = sprintf(dst, "\\u%04x", c);
                dst += n;
            } else {
                *dst++ = (char)c;
            }
            break;
        }
    }
    *dst = '\0';
    return escaped;
}

// Function to compare two pointers for equality
bool nostr_pointer_values_equal(const void *a, const void *b, size_t size) {
    if (a == NULL && b == NULL) {
        return true;
    }
    if (a != NULL && b != NULL) {
        return memcmp(a, b, size) == 0;
    }
    return false;
}

int64_t nostr_sub_id_to_serial(const char *sub_id) {
    if (!sub_id || !*sub_id) return -1;

    // Accept either plain numeric IDs ("123") or numeric prefix followed by ':' ("123:foo")
    char *endptr = NULL;
    long long val = strtoll(sub_id, &endptr, 10);
    if (endptr == sub_id) {
        return -1; // no digits parsed
    }
    if (*endptr == '\0' || *endptr == ':') {
        return (int64_t)val;
    }
    return -1; // unexpected suffix
}

// Fast hex helpers
static inline int hex_nibble(unsigned char c) {
    // 0-9 => 0..9, A-F/a-f => 10..15, else -1
    if (c >= '0' && c <= '9') return (int)(c - '0');
    c |= 0x20; // fold to lowercase
    if (c >= 'a' && c <= 'f') return 10 + (int)(c - 'a');
    return -1;
}

// Convert hex string to binary (lower/upper accepted). Returns false on malformed input.
bool nostr_hex2bin(unsigned char *bin, const char *hex, size_t bin_len) {
    if (!bin || !hex) return false;
    size_t hex_len = strlen(hex);
    if (hex_len != bin_len * 2) return false;
    const unsigned char *p = (const unsigned char *)hex;
    for (size_t i = 0; i < bin_len; ++i) {
        int hi = hex_nibble(p[0]); if (hi < 0) return false;
        int lo = hex_nibble(p[1]); if (lo < 0) return false;
        bin[i] = (unsigned char)((hi << 4) | lo);
        p += 2;
    }
    return true;
}

// Convert binary to lowercase hex string. Caller must free.
char *nostr_bin2hex(const unsigned char *bin, size_t len) {
    if (!bin) return NULL;
    static const char hexdig[16] = {
        '0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'
    };
    size_t out_len = len * 2;
    char *out = (char *)malloc(out_len + 1);
    if (!out) return NULL;
    for (size_t i = 0, j = 0; i < len; ++i) {
        unsigned char b = bin[i];
        out[j++] = hexdig[b >> 4];
        out[j++] = hexdig[b & 0x0F];
    }
    out[out_len] = '\0';
    return out;
}
