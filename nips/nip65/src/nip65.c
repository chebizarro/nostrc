/**
 * NIP-65: Relay List Metadata Implementation
 */
#include "nostr/nip65/nip65.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/* --------------------------------------------------------------------------
 * Internal Helpers
 * -------------------------------------------------------------------------- */

static char *hex_from_32(const unsigned char bin[32]) {
    static const char *hex_chars = "0123456789abcdef";
    char *out = (char *)malloc(65);
    if (!out) return NULL;
    for (size_t i = 0; i < 32; ++i) {
        out[2*i]   = hex_chars[(bin[i] >> 4) & 0xF];
        out[2*i+1] = hex_chars[bin[i] & 0xF];
    }
    out[64] = '\0';
    return out;
}

static char *str_tolower(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < len; ++i) {
        out[i] = (char)tolower((unsigned char)s[i]);
    }
    out[len] = '\0';
    return out;
}

static int urls_equal(const char *a, const char *b) {
    if (!a || !b) return 0;
    char *na = nostr_nip65_normalize_url(a);
    char *nb = nostr_nip65_normalize_url(b);
    int eq = (na && nb && strcmp(na, nb) == 0);
    free(na);
    free(nb);
    return eq;
}

/* --------------------------------------------------------------------------
 * Relay Entry Functions
 * -------------------------------------------------------------------------- */

NostrRelayEntry *nostr_nip65_entry_new(const char *url,
                                       NostrRelayPermission permission) {
    if (!url || !*url) return NULL;

    NostrRelayEntry *entry = calloc(1, sizeof(NostrRelayEntry));
    if (!entry) return NULL;

    entry->url = nostr_nip65_normalize_url(url);
    if (!entry->url) {
        free(entry);
        return NULL;
    }

    entry->permission = permission;
    return entry;
}

void nostr_nip65_entry_free(NostrRelayEntry *entry) {
    if (!entry) return;
    free(entry->url);
    free(entry);
}

NostrRelayEntry *nostr_nip65_entry_copy(const NostrRelayEntry *entry) {
    if (!entry) return NULL;
    return nostr_nip65_entry_new(entry->url, entry->permission);
}

bool nostr_nip65_entry_is_readable(const NostrRelayEntry *entry) {
    if (!entry) return false;
    return entry->permission == NOSTR_RELAY_PERM_READ ||
           entry->permission == NOSTR_RELAY_PERM_READWRITE;
}

bool nostr_nip65_entry_is_writable(const NostrRelayEntry *entry) {
    if (!entry) return false;
    return entry->permission == NOSTR_RELAY_PERM_WRITE ||
           entry->permission == NOSTR_RELAY_PERM_READWRITE;
}

/* --------------------------------------------------------------------------
 * Relay List Functions
 * -------------------------------------------------------------------------- */

NostrRelayList *nostr_nip65_list_new(void) {
    NostrRelayList *list = calloc(1, sizeof(NostrRelayList));
    return list;
}

void nostr_nip65_list_free(NostrRelayList *list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; ++i) {
        free(list->entries[i].url);
    }
    free(list->entries);
    free(list);
}

NostrRelayList *nostr_nip65_list_copy(const NostrRelayList *list) {
    if (!list) return NULL;

    NostrRelayList *copy = nostr_nip65_list_new();
    if (!copy) return NULL;

    if (list->count == 0) return copy;

    copy->entries = calloc(list->count, sizeof(NostrRelayEntry));
    if (!copy->entries) {
        free(copy);
        return NULL;
    }

    for (size_t i = 0; i < list->count; ++i) {
        copy->entries[i].url = strdup(list->entries[i].url);
        if (!copy->entries[i].url) {
            nostr_nip65_list_free(copy);
            return NULL;
        }
        copy->entries[i].permission = list->entries[i].permission;
    }
    copy->count = list->count;

    return copy;
}

int nostr_nip65_add_relay(NostrRelayList *list,
                          const char *url,
                          NostrRelayPermission permission) {
    if (!list || !url || !*url) return -EINVAL;

    /* Check if URL is valid */
    if (!nostr_nip65_is_valid_relay_url(url)) return -EINVAL;

    /* Check if URL already exists */
    NostrRelayEntry *existing = nostr_nip65_find_relay(list, url);
    if (existing) {
        /* Update permission */
        existing->permission = permission;
        return 0;
    }

    /* Add new entry */
    NostrRelayEntry *new_entries = realloc(list->entries,
                                           (list->count + 1) * sizeof(NostrRelayEntry));
    if (!new_entries) return -ENOMEM;

    list->entries = new_entries;
    list->entries[list->count].url = nostr_nip65_normalize_url(url);
    if (!list->entries[list->count].url) return -ENOMEM;
    list->entries[list->count].permission = permission;
    list->count++;

    return 0;
}

int nostr_nip65_remove_relay(NostrRelayList *list, const char *url) {
    if (!list || !url) return -EINVAL;

    for (size_t i = 0; i < list->count; ++i) {
        if (urls_equal(list->entries[i].url, url)) {
            free(list->entries[i].url);
            /* Shift remaining entries */
            for (size_t j = i; j < list->count - 1; ++j) {
                list->entries[j] = list->entries[j + 1];
            }
            list->count--;
            return 0;
        }
    }

    return -ENOENT;
}

NostrRelayEntry *nostr_nip65_find_relay(const NostrRelayList *list,
                                        const char *url) {
    if (!list || !url) return NULL;

    for (size_t i = 0; i < list->count; ++i) {
        if (urls_equal(list->entries[i].url, url)) {
            return &list->entries[i];
        }
    }

    return NULL;
}

char **nostr_nip65_get_read_relays(const NostrRelayList *list, size_t *out_count) {
    size_t count = 0;

    if (list) {
        for (size_t i = 0; i < list->count; ++i) {
            if (nostr_nip65_entry_is_readable(&list->entries[i])) {
                count++;
            }
        }
    }

    char **result = calloc(count + 1, sizeof(char *));
    if (!result) {
        if (out_count) *out_count = 0;
        return NULL;
    }

    size_t idx = 0;
    if (list) {
        for (size_t i = 0; i < list->count; ++i) {
            if (nostr_nip65_entry_is_readable(&list->entries[i])) {
                result[idx] = strdup(list->entries[i].url);
                if (!result[idx]) {
                    nostr_nip65_free_string_array(result);
                    if (out_count) *out_count = 0;
                    return NULL;
                }
                idx++;
            }
        }
    }

    if (out_count) *out_count = count;
    return result;
}

char **nostr_nip65_get_write_relays(const NostrRelayList *list, size_t *out_count) {
    size_t count = 0;

    if (list) {
        for (size_t i = 0; i < list->count; ++i) {
            if (nostr_nip65_entry_is_writable(&list->entries[i])) {
                count++;
            }
        }
    }

    char **result = calloc(count + 1, sizeof(char *));
    if (!result) {
        if (out_count) *out_count = 0;
        return NULL;
    }

    size_t idx = 0;
    if (list) {
        for (size_t i = 0; i < list->count; ++i) {
            if (nostr_nip65_entry_is_writable(&list->entries[i])) {
                result[idx] = strdup(list->entries[i].url);
                if (!result[idx]) {
                    nostr_nip65_free_string_array(result);
                    if (out_count) *out_count = 0;
                    return NULL;
                }
                idx++;
            }
        }
    }

    if (out_count) *out_count = count;
    return result;
}

/* --------------------------------------------------------------------------
 * Event Building and Parsing
 * -------------------------------------------------------------------------- */

int nostr_nip65_create_relay_list(NostrEvent *ev,
                                  const unsigned char author_pk[32],
                                  const NostrRelayList *list,
                                  uint32_t created_at) {
    if (!ev || !author_pk) return -EINVAL;

    /* Set kind 10002 */
    nostr_event_set_kind(ev, NOSTR_NIP65_KIND);
    nostr_event_set_created_at(ev, (int64_t)created_at);

    /* Set author pubkey */
    char *author_hex = hex_from_32(author_pk);
    if (!author_hex) return -ENOMEM;
    nostr_event_set_pubkey(ev, author_hex);
    free(author_hex);

    /* Content is always empty for NIP-65 */
    nostr_event_set_content(ev, "");

    /* Build tags from relay list */
    NostrTags *tags = nostr_tags_new(0);
    if (!tags) return -ENOMEM;

    if (list) {
        for (size_t i = 0; i < list->count; ++i) {
            const NostrRelayEntry *e = &list->entries[i];
            if (!e->url) continue;

            const char *marker = nostr_nip65_permission_to_string(e->permission);
            NostrTag *tag;
            if (marker) {
                tag = nostr_tag_new("r", e->url, marker, NULL);
            } else {
                tag = nostr_tag_new("r", e->url, NULL);
            }
            if (tag) {
                nostr_tags_append(tags, tag);
            }
        }
    }

    nostr_event_set_tags(ev, tags);
    return 0;
}

int nostr_nip65_parse_relay_list(const NostrEvent *ev, NostrRelayList **out) {
    if (!ev || !out) return -EINVAL;

    /* Check kind */
    int kind = nostr_event_get_kind(ev);
    if (kind != NOSTR_NIP65_KIND) return -ENOENT;

    *out = nostr_nip65_list_new();
    if (!*out) return -ENOMEM;

    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return 0;

    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;

        const char *key = nostr_tag_get(t, 0);
        if (!key || strcmp(key, "r") != 0) continue;

        const char *url = nostr_tag_get(t, 1);
        if (!url || !*url) continue;

        /* Validate URL */
        if (!nostr_nip65_is_valid_relay_url(url)) continue;

        /* Parse permission marker */
        NostrRelayPermission perm = NOSTR_RELAY_PERM_READWRITE;
        if (nostr_tag_size(t) >= 3) {
            const char *marker = nostr_tag_get(t, 2);
            perm = nostr_nip65_permission_from_string(marker);
        }

        nostr_nip65_add_relay(*out, url, perm);
    }

    return 0;
}

int nostr_nip65_update_relay_list(NostrEvent *ev, const NostrRelayList *list) {
    if (!ev) return -EINVAL;

    /* Build new tags */
    NostrTags *tags = nostr_tags_new(0);
    if (!tags) return -ENOMEM;

    if (list) {
        for (size_t i = 0; i < list->count; ++i) {
            const NostrRelayEntry *e = &list->entries[i];
            if (!e->url) continue;

            const char *marker = nostr_nip65_permission_to_string(e->permission);
            NostrTag *tag;
            if (marker) {
                tag = nostr_tag_new("r", e->url, marker, NULL);
            } else {
                tag = nostr_tag_new("r", e->url, NULL);
            }
            if (tag) {
                nostr_tags_append(tags, tag);
            }
        }
    }

    nostr_event_set_tags(ev, tags);
    return 0;
}

/* --------------------------------------------------------------------------
 * Utility Functions
 * -------------------------------------------------------------------------- */

char *nostr_nip65_normalize_url(const char *url) {
    if (!url || !*url) return NULL;

    /* Skip leading whitespace */
    while (*url && isspace((unsigned char)*url)) url++;
    if (!*url) return NULL;

    /* Check scheme */
    const char *scheme_end = strstr(url, "://");
    if (!scheme_end) return NULL;

    size_t scheme_len = scheme_end - url;
    char *scheme = str_tolower(url);
    if (!scheme) return NULL;
    scheme[scheme_len] = '\0';

    if (strcmp(scheme, "ws") != 0 && strcmp(scheme, "wss") != 0) {
        free(scheme);
        return NULL;
    }

    /* Find host start */
    const char *host_start = scheme_end + 3;
    if (!*host_start) {
        free(scheme);
        return NULL;
    }

    /* Find host end (port or path) */
    const char *host_end = host_start;
    while (*host_end && *host_end != ':' && *host_end != '/' && *host_end != '?' && *host_end != '#') {
        host_end++;
    }

    size_t host_len = host_end - host_start;
    if (host_len == 0) {
        free(scheme);
        return NULL;
    }

    /* Extract and lowercase host */
    char *host = malloc(host_len + 1);
    if (!host) {
        free(scheme);
        return NULL;
    }
    for (size_t i = 0; i < host_len; ++i) {
        host[i] = (char)tolower((unsigned char)host_start[i]);
    }
    host[host_len] = '\0';

    /* Find port if present */
    char *port = NULL;
    const char *path_start = host_end;
    if (*host_end == ':') {
        const char *port_start = host_end + 1;
        const char *port_end = port_start;
        while (*port_end && *port_end != '/' && *port_end != '?' && *port_end != '#') {
            port_end++;
        }
        size_t port_len = port_end - port_start;
        if (port_len > 0) {
            port = malloc(port_len + 1);
            if (port) {
                memcpy(port, port_start, port_len);
                port[port_len] = '\0';
            }
        }
        path_start = port_end;
    }

    /* Extract path (excluding trailing slash) */
    char *path = NULL;
    if (*path_start == '/') {
        const char *path_end = path_start;
        while (*path_end && *path_end != '?' && *path_end != '#') {
            path_end++;
        }
        size_t path_len = path_end - path_start;
        /* Skip single trailing slash */
        if (path_len > 1 || (path_len == 1 && path_start[0] != '/')) {
            /* Remove trailing slash */
            while (path_len > 1 && path_start[path_len - 1] == '/') {
                path_len--;
            }
            if (path_len > 1) {
                path = malloc(path_len + 1);
                if (path) {
                    memcpy(path, path_start, path_len);
                    path[path_len] = '\0';
                }
            }
        }
    }

    /* Build normalized URL */
    size_t result_len = strlen(scheme) + 3 + strlen(host);
    if (port) result_len += 1 + strlen(port);
    if (path) result_len += strlen(path);

    char *result = malloc(result_len + 1);
    if (!result) {
        free(scheme);
        free(host);
        free(port);
        free(path);
        return NULL;
    }

    if (port && path) {
        snprintf(result, result_len + 1, "%s://%s:%s%s", scheme, host, port, path);
    } else if (port) {
        snprintf(result, result_len + 1, "%s://%s:%s", scheme, host, port);
    } else if (path) {
        snprintf(result, result_len + 1, "%s://%s%s", scheme, host, path);
    } else {
        snprintf(result, result_len + 1, "%s://%s", scheme, host);
    }

    free(scheme);
    free(host);
    free(port);
    free(path);

    return result;
}

bool nostr_nip65_is_valid_relay_url(const char *url) {
    if (!url || !*url) return false;

    /* Skip whitespace */
    while (*url && isspace((unsigned char)*url)) url++;

    /* Check for ws:// or wss:// */
    if (strncmp(url, "ws://", 5) != 0 && strncmp(url, "wss://", 6) != 0) {
        return false;
    }

    /* Find host */
    const char *host_start = (url[2] == 's') ? url + 6 : url + 5;
    if (!*host_start) return false;

    /* Check for at least one character in host */
    const char *host_end = host_start;
    while (*host_end && *host_end != ':' && *host_end != '/' && *host_end != '?' && *host_end != '#') {
        host_end++;
    }

    return (host_end > host_start);
}

const char *nostr_nip65_permission_to_string(NostrRelayPermission permission) {
    switch (permission) {
        case NOSTR_RELAY_PERM_READ: return "read";
        case NOSTR_RELAY_PERM_WRITE: return "write";
        case NOSTR_RELAY_PERM_READWRITE:
        default: return NULL;
    }
}

NostrRelayPermission nostr_nip65_permission_from_string(const char *str) {
    if (!str || !*str) return NOSTR_RELAY_PERM_READWRITE;
    if (strcmp(str, "read") == 0) return NOSTR_RELAY_PERM_READ;
    if (strcmp(str, "write") == 0) return NOSTR_RELAY_PERM_WRITE;
    return NOSTR_RELAY_PERM_READWRITE;
}

void nostr_nip65_free_string_array(char **arr) {
    if (!arr) return;
    for (size_t i = 0; arr[i]; ++i) {
        free(arr[i]);
    }
    free(arr);
}
