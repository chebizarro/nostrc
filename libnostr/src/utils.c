#include "utils.h"
#include <ctype.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helper function to trim leading and trailing whitespace
static char *trim_whitespace(char *str) {
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
char *normalize_url(const char *u) {
    if (!u || strlen(u) == 0) {
        return strdup("");
    }

    char *url = strdup(u);
    if (!url)
        return NULL;

    url = trim_whitespace(url);

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
char *normalize_ok_message(const char *reason, const char *prefix) {
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
pthread_mutex_t named_mutex_pool[MAX_LOCKS] = {PTHREAD_MUTEX_INITIALIZER};

// Simple hash function for demonstration
uint64_t memhash(const char *data, size_t len) {
    uint64_t hash = 5381;
    for (size_t i = 0; i < len; i++) {
        hash = ((hash << 5) + hash) + data[i];
    }
    return hash;
}

// Function to acquire a named lock
void named_lock(const char *name, void (*critical_section)(void *), void *arg) {
    uint64_t hash = memhash(name, strlen(name));
    size_t idx = hash % MAX_LOCKS;
    pthread_mutex_lock(&named_mutex_pool[idx]);
    critical_section(arg);
    pthread_mutex_unlock(&named_mutex_pool[idx]);
}

// Function to compare two arrays for similarity
bool similar(const int *as, size_t as_len, const int *bs, size_t bs_len) {
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

// Function to escape a string for JSON encoding
char *escape_string(const char *s) {
    size_t len = strlen(s);
    size_t escaped_len = len * 2 + 2; // rough estimate
    char *escaped = (char *)malloc(escaped_len);
    if (!escaped) {
        return NULL;
    }

    char *dst = escaped;
    *dst++ = '"';
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        switch (c) {
        case '"':
            *dst++ = '\\';
            *dst++ = '"';
            break;
        case '\\':
            *dst++ = '\\';
            *dst++ = '\\';
            break;
        case '\b':
            *dst++ = '\\';
            *dst++ = 'b';
            break;
        case '\t':
            *dst++ = '\\';
            *dst++ = 't';
            break;
        case '\n':
            *dst++ = '\\';
            *dst++ = 'n';
            break;
        case '\f':
            *dst++ = '\\';
            *dst++ = 'f';
            break;
        case '\r':
            *dst++ = '\\';
            *dst++ = 'r';
            break;
        default:
            if (iscntrl(c)) {
                dst += sprintf(dst, "\\u%04x", c);
            } else {
                *dst++ = c;
            }
            break;
        }
    }
    *dst++ = '"';
    *dst = '\0';

    return escaped;
}

// Function to compare two pointers for equality
bool are_pointer_values_equal(const void *a, const void *b, size_t size) {
    if (a == NULL && b == NULL) {
        return true;
    }
    if (a != NULL && b != NULL) {
        return memcmp(a, b, size) == 0;
    }
    return false;
}

int64_t sub_id_to_serial(const char *sub_id) {
    if (!sub_id) return -1;

    // Find the index of the colon character
    char *colon_pos = strchr(sub_id, ':');
    if (!colon_pos) {
        return -1;  // Colon not found
    }

    // Convert the part of the string before the colon to an integer
    char *endptr;
    int64_t serial_id = strtoll(sub_id, &endptr, 10);

    // Check if conversion was successful and if the next character is the colon
    if (*endptr != ':' || endptr != colon_pos) {
        return -1;  // Invalid number or improperly formatted string
    }

    return serial_id;
}

// Convert hex string to binary
bool hex2bin(unsigned char *bin, const char *hex, size_t bin_len) {
    if (strlen(hex) != bin_len * 2)
        return false;
    for (size_t i = 0; i < bin_len; i++) {
        sscanf(hex + 2 * i, "%2hhx", &bin[i]);
    }
    return true;
}

// Escape special characters in the string according to RFC8259
char *escape_string(const char *str) {
    size_t len = strlen(str);
    size_t capacity = len * 2;            // Start with enough space
    char *escaped = malloc(capacity + 1); // Allocate buffer
    if (!escaped)
        return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (j + 6 > capacity) { // Make sure we have enough space
            capacity *= 2;
            escaped = realloc(escaped, capacity + 1);
            if (!escaped)
                return NULL;
        }

        switch (str[i]) {
        case '\"':
            escaped[j++] = '\\';
            escaped[j++] = '\"';
            break;
        case '\\':
            escaped[j++] = '\\';
            escaped[j++] = '\\';
            break;
        case '\b':
            escaped[j++] = '\\';
            escaped[j++] = 'b';
            break;
        case '\f':
            escaped[j++] = '\\';
            escaped[j++] = 'f';
            break;
        case '\n':
            escaped[j++] = '\\';
            escaped[j++] = 'n';
            break;
        case '\r':
            escaped[j++] = '\\';
            escaped[j++] = 'r';
            break;
        case '\t':
            escaped[j++] = '\\';
            escaped[j++] = 't';
            break;
        default:
            escaped[j++] = str[i]; // No escape needed
            break;
        }
    }

    escaped[j] = '\0'; // Null-terminate the string
    return escaped;
}
