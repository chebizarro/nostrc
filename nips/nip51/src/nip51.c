/**
 * NIP-51: User Lists
 *
 * Implements mute lists, bookmarks, and other user lists with
 * support for private (NIP-44 encrypted) entries.
 */

#include "nostr/nip51/nip51.h"
#include "nostr/nip44/nip44.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr-kinds.h"
#include "nostr-keys.h"
#include "nostr-utils.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INITIAL_CAPACITY 8

/* ---- Memory Management ---- */

NostrList *nostr_nip51_list_new(void) {
    NostrList *list = calloc(1, sizeof(NostrList));
    if (!list) return NULL;

    list->entries = calloc(INITIAL_CAPACITY, sizeof(NostrListEntry *));
    if (!list->entries) {
        free(list);
        return NULL;
    }

    list->capacity = INITIAL_CAPACITY;
    list->count = 0;
    return list;
}

void nostr_nip51_list_free(NostrList *list) {
    if (!list) return;

    for (size_t i = 0; i < list->count; i++) {
        nostr_nip51_entry_free(list->entries[i]);
    }
    free(list->entries);
    free(list->identifier);
    free(list->title);
    free(list->description);
    free(list);
}

NostrListEntry *nostr_nip51_entry_new(const char *tag_name,
                                       const char *value,
                                       const char *extra,
                                       bool is_private) {
    if (!tag_name || !value) return NULL;

    NostrListEntry *entry = calloc(1, sizeof(NostrListEntry));
    if (!entry) return NULL;

    entry->tag_name = strdup(tag_name);
    entry->value = strdup(value);
    entry->extra = extra ? strdup(extra) : NULL;
    entry->is_private = is_private;

    if (!entry->tag_name || !entry->value) {
        nostr_nip51_entry_free(entry);
        return NULL;
    }

    return entry;
}

void nostr_nip51_entry_free(NostrListEntry *entry) {
    if (!entry) return;
    free(entry->tag_name);
    free(entry->value);
    free(entry->extra);
    free(entry);
}

void nostr_nip51_list_add_entry(NostrList *list, NostrListEntry *entry) {
    if (!list || !entry) return;

    /* Grow array if needed */
    if (list->count >= list->capacity) {
        size_t new_cap = list->capacity * 2;
        NostrListEntry **new_entries = realloc(list->entries,
                                                 new_cap * sizeof(NostrListEntry *));
        if (!new_entries) return;
        list->entries = new_entries;
        list->capacity = new_cap;
    }

    list->entries[list->count++] = entry;
}

void nostr_nip51_list_set_identifier(NostrList *list, const char *identifier) {
    if (!list) return;
    free(list->identifier);
    list->identifier = identifier ? strdup(identifier) : NULL;
}

void nostr_nip51_list_set_title(NostrList *list, const char *title) {
    if (!list) return;
    free(list->title);
    list->title = title ? strdup(title) : NULL;
}

/* ---- Convenience Entry Builders ---- */

void nostr_nip51_mute_user(NostrList *list, const char *pubkey_hex, bool is_private) {
    NostrListEntry *entry = nostr_nip51_entry_new("p", pubkey_hex, NULL, is_private);
    if (entry) nostr_nip51_list_add_entry(list, entry);
}

void nostr_nip51_mute_word(NostrList *list, const char *word, bool is_private) {
    NostrListEntry *entry = nostr_nip51_entry_new("word", word, NULL, is_private);
    if (entry) nostr_nip51_list_add_entry(list, entry);
}

void nostr_nip51_mute_hashtag(NostrList *list, const char *hashtag, bool is_private) {
    NostrListEntry *entry = nostr_nip51_entry_new("t", hashtag, NULL, is_private);
    if (entry) nostr_nip51_list_add_entry(list, entry);
}

void nostr_nip51_mute_event(NostrList *list, const char *event_id_hex, bool is_private) {
    NostrListEntry *entry = nostr_nip51_entry_new("e", event_id_hex, NULL, is_private);
    if (entry) nostr_nip51_list_add_entry(list, entry);
}

void nostr_nip51_bookmark_event(NostrList *list,
                                  const char *event_id_hex,
                                  const char *relay_hint,
                                  bool is_private) {
    NostrListEntry *entry = nostr_nip51_entry_new("e", event_id_hex, relay_hint, is_private);
    if (entry) nostr_nip51_list_add_entry(list, entry);
}

void nostr_nip51_bookmark_url(NostrList *list, const char *url, bool is_private) {
    NostrListEntry *entry = nostr_nip51_entry_new("r", url, NULL, is_private);
    if (entry) nostr_nip51_list_add_entry(list, entry);
}

/* ---- Private Entry Encryption ---- */

/**
 * Serialize entries to JSON array: [["tag", "val"], ["tag", "val", "extra"], ...]
 */
static char *entries_to_json(NostrListEntry **entries, size_t count) {
    if (count == 0) return strdup("[]");

    /* Estimate size: ~100 bytes per entry */
    size_t buf_size = count * 128 + 16;
    char *buf = malloc(buf_size);
    if (!buf) return NULL;

    size_t pos = 0;
    buf[pos++] = '[';

    for (size_t i = 0; i < count; i++) {
        NostrListEntry *e = entries[i];
        if (!e) continue;

        if (i > 0) buf[pos++] = ',';

        /* Build tag array: ["tag", "value"] or ["tag", "value", "extra"] */
        int written;
        if (e->extra) {
            written = snprintf(buf + pos, buf_size - pos,
                              "[\"%s\",\"%s\",\"%s\"]",
                              e->tag_name, e->value, e->extra);
        } else {
            written = snprintf(buf + pos, buf_size - pos,
                              "[\"%s\",\"%s\"]",
                              e->tag_name, e->value);
        }

        if (written < 0 || (size_t)written >= buf_size - pos) {
            /* Grow buffer */
            buf_size *= 2;
            char *new_buf = realloc(buf, buf_size);
            if (!new_buf) {
                free(buf);
                return NULL;
            }
            buf = new_buf;
            i--;  /* Retry this entry */
            continue;
        }
        pos += written;
    }

    buf[pos++] = ']';
    buf[pos] = '\0';
    return buf;
}

/**
 * Simple JSON array parser for [["tag", "val"], ...] format
 */
static NostrListEntry **json_to_entries(const char *json, size_t *count_out) {
    *count_out = 0;
    if (!json || json[0] != '[') return NULL;

    /* Count entries (rough estimate by counting '[' after first) */
    size_t capacity = 8;
    NostrListEntry **entries = calloc(capacity, sizeof(NostrListEntry *));
    if (!entries) return NULL;

    size_t count = 0;
    const char *p = json + 1;  /* Skip opening '[' */

    while (*p) {
        /* Skip whitespace and commas */
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t') p++;

        if (*p == ']') break;  /* End of outer array */

        if (*p != '[') {
            p++;
            continue;
        }

        /* Parse inner array ["tag", "val", "extra"?] */
        p++;  /* Skip '[' */

        char *tag = NULL, *val = NULL, *extra = NULL;
        int field = 0;

        while (*p && *p != ']') {
            while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\t') p++;

            if (*p == '"') {
                p++;  /* Skip opening quote */
                const char *start = p;
                while (*p && *p != '"') p++;
                size_t len = p - start;

                char *str = malloc(len + 1);
                if (str) {
                    memcpy(str, start, len);
                    str[len] = '\0';

                    if (field == 0) tag = str;
                    else if (field == 1) val = str;
                    else if (field == 2) extra = str;
                    else free(str);

                    field++;
                }

                if (*p == '"') p++;  /* Skip closing quote */
            } else if (*p && *p != ']') {
                p++;
            }
        }

        if (*p == ']') p++;  /* Skip closing ']' of inner array */

        /* Create entry if we have tag and value */
        if (tag && val) {
            NostrListEntry *entry = nostr_nip51_entry_new(tag, val, extra, true);
            if (entry) {
                /* Grow array if needed */
                if (count >= capacity) {
                    capacity *= 2;
                    NostrListEntry **new_entries = realloc(entries,
                                                            capacity * sizeof(NostrListEntry *));
                    if (!new_entries) {
                        nostr_nip51_entry_free(entry);
                        break;
                    }
                    entries = new_entries;
                }
                entries[count++] = entry;
            }
        }

        free(tag);
        free(val);
        free(extra);
    }

    *count_out = count;
    return entries;
}

char *nostr_nip51_encrypt_private_entries(NostrListEntry **entries,
                                           size_t count,
                                           const char *sk_hex) {
    if (!entries || count == 0 || !sk_hex) return NULL;

    /* Serialize to JSON */
    char *json = entries_to_json(entries, count);
    if (!json) return NULL;

    /* Get public key from secret key */
    char *pk_hex = nostr_key_get_public(sk_hex);
    if (!pk_hex) {
        free(json);
        return NULL;
    }

    /* Convert keys to binary */
    unsigned char sk[32], pk[32];
    if (!nostr_hex2bin(sk, sk_hex, 32) || !nostr_hex2bin(pk, pk_hex, 32)) {
        memset(sk, 0, 32);
        free(pk_hex);
        free(json);
        return NULL;
    }

    /* Encrypt with NIP-44 (self-encryption) */
    char *encrypted = NULL;
    int rc = nostr_nip44_encrypt_v2(sk, pk,
                                     (const uint8_t *)json, strlen(json),
                                     &encrypted);

    /* Cleanup */
    memset(sk, 0, 32);
    free(pk_hex);
    free(json);

    if (rc != 0) return NULL;
    return encrypted;
}

NostrListEntry **nostr_nip51_decrypt_private_entries(const char *content,
                                                       const char *sk_hex,
                                                       size_t *count_out) {
    *count_out = 0;
    if (!content || !*content || !sk_hex) return NULL;

    /* Get public key from secret key */
    char *pk_hex = nostr_key_get_public(sk_hex);
    if (!pk_hex) return NULL;

    /* Convert keys to binary */
    unsigned char sk[32], pk[32];
    if (!nostr_hex2bin(sk, sk_hex, 32) || !nostr_hex2bin(pk, pk_hex, 32)) {
        memset(sk, 0, 32);
        free(pk_hex);
        return NULL;
    }

    /* Decrypt with NIP-44 */
    uint8_t *decrypted = NULL;
    size_t decrypted_len = 0;
    int rc = nostr_nip44_decrypt_v2(sk, pk, content, &decrypted, &decrypted_len);

    memset(sk, 0, 32);
    free(pk_hex);

    if (rc != 0 || !decrypted) return NULL;

    /* Null-terminate */
    char *json = malloc(decrypted_len + 1);
    if (!json) {
        free(decrypted);
        return NULL;
    }
    memcpy(json, decrypted, decrypted_len);
    json[decrypted_len] = '\0';
    free(decrypted);

    /* Parse JSON to entries */
    NostrListEntry **entries = json_to_entries(json, count_out);
    free(json);

    return entries;
}

void nostr_nip51_free_entries(NostrListEntry **entries, size_t count) {
    if (!entries) return;
    for (size_t i = 0; i < count; i++) {
        nostr_nip51_entry_free(entries[i]);
    }
    free(entries);
}

/* ---- Event Creation ---- */

static int64_t get_current_time(void) {
    return (int64_t)time(NULL);
}

NostrEvent *nostr_nip51_create_list(int kind, NostrList *list, const char *sk_hex) {
    if (!list || !sk_hex) return NULL;

    /* Get public key */
    char *pk_hex = nostr_key_get_public(sk_hex);
    if (!pk_hex) return NULL;

    NostrEvent *event = nostr_event_new();
    if (!event) {
        free(pk_hex);
        return NULL;
    }

    nostr_event_set_kind(event, kind);
    nostr_event_set_pubkey(event, pk_hex);
    nostr_event_set_created_at(event, get_current_time());

    /* Separate public and private entries */
    NostrListEntry **private_entries = NULL;
    size_t private_count = 0;
    size_t private_cap = 0;

    NostrTags *tags = nostr_tags_new(0);
    if (!tags) {
        free(pk_hex);
        nostr_event_free(event);
        return NULL;
    }

    /* Add d-tag for addressable lists (kind 30000+) */
    if (kind >= 30000 && list->identifier) {
        NostrTag *d_tag = nostr_tag_new("d", list->identifier, NULL);
        if (d_tag) nostr_tags_append(tags, d_tag);
    }

    /* Add title if present */
    if (list->title) {
        NostrTag *title_tag = nostr_tag_new("title", list->title, NULL);
        if (title_tag) nostr_tags_append(tags, title_tag);
    }

    /* Add description if present */
    if (list->description) {
        NostrTag *desc_tag = nostr_tag_new("description", list->description, NULL);
        if (desc_tag) nostr_tags_append(tags, desc_tag);
    }

    /* Process entries */
    for (size_t i = 0; i < list->count; i++) {
        NostrListEntry *e = list->entries[i];
        if (!e) continue;

        if (e->is_private) {
            /* Collect private entries for encryption */
            if (private_count >= private_cap) {
                private_cap = private_cap ? private_cap * 2 : 8;
                NostrListEntry **new_priv = realloc(private_entries,
                                                     private_cap * sizeof(NostrListEntry *));
                if (!new_priv) continue;
                private_entries = new_priv;
            }
            private_entries[private_count++] = e;
        } else {
            /* Add public entry as tag */
            NostrTag *tag;
            if (e->extra) {
                tag = nostr_tag_new(e->tag_name, e->value, e->extra, NULL);
            } else {
                tag = nostr_tag_new(e->tag_name, e->value, NULL);
            }
            if (tag) nostr_tags_append(tags, tag);
        }
    }

    nostr_event_set_tags(event, tags);

    /* Encrypt private entries if any */
    if (private_count > 0) {
        char *encrypted = nostr_nip51_encrypt_private_entries(private_entries,
                                                                private_count,
                                                                sk_hex);
        if (encrypted) {
            nostr_event_set_content(event, encrypted);
            free(encrypted);
        } else {
            nostr_event_set_content(event, "");
        }
    } else {
        nostr_event_set_content(event, "");
    }

    free(private_entries);
    free(pk_hex);

    /* Sign the event */
    if (nostr_event_sign(event, sk_hex) != 0) {
        nostr_event_free(event);
        return NULL;
    }

    return event;
}

NostrEvent *nostr_nip51_create_mute_list(NostrList *list, const char *sk_hex) {
    return nostr_nip51_create_list(NOSTR_KIND_MUTE_LIST, list, sk_hex);
}

NostrEvent *nostr_nip51_create_bookmark_list(NostrList *list, const char *sk_hex) {
    return nostr_nip51_create_list(NOSTR_KIND_BOOKMARK_LIST, list, sk_hex);
}

NostrEvent *nostr_nip51_create_pin_list(NostrList *list, const char *sk_hex) {
    return nostr_nip51_create_list(NOSTR_KIND_PIN_LIST, list, sk_hex);
}

/* ---- Event Parsing ---- */

NostrList *nostr_nip51_parse_list(NostrEvent *event, const char *sk_hex) {
    if (!event) return NULL;

    NostrList *list = nostr_nip51_list_new();
    if (!list) return NULL;

    /* Parse public entries from tags */
    NostrTags *tags = nostr_event_get_tags(event);
    if (tags) {
        size_t tag_count = nostr_tags_size(tags);
        for (size_t i = 0; i < tag_count; i++) {
            NostrTag *tag = nostr_tags_get(tags, i);
            if (!tag || nostr_tag_size(tag) < 2) continue;

            const char *key = nostr_tag_get(tag, 0);
            const char *value = nostr_tag_get(tag, 1);
            if (!key || !value) continue;

            /* Skip metadata tags */
            if (strcmp(key, "d") == 0) {
                nostr_nip51_list_set_identifier(list, value);
                continue;
            }
            if (strcmp(key, "title") == 0) {
                nostr_nip51_list_set_title(list, value);
                continue;
            }
            if (strcmp(key, "description") == 0) {
                free(list->description);
                list->description = strdup(value);
                continue;
            }

            /* Create entry for list item tags */
            if (strcmp(key, "p") == 0 || strcmp(key, "e") == 0 ||
                strcmp(key, "t") == 0 || strcmp(key, "word") == 0 ||
                strcmp(key, "a") == 0 || strcmp(key, "r") == 0) {

                const char *extra = nostr_tag_size(tag) > 2 ?
                                    nostr_tag_get(tag, 2) : NULL;
                NostrListEntry *entry = nostr_nip51_entry_new(key, value, extra, false);
                if (entry) nostr_nip51_list_add_entry(list, entry);
            }
        }
    }

    /* Parse private entries from encrypted content */
    if (sk_hex) {
        const char *content = nostr_event_get_content(event);
        if (content && *content) {
            size_t priv_count = 0;
            NostrListEntry **priv_entries = nostr_nip51_decrypt_private_entries(
                content, sk_hex, &priv_count);

            if (priv_entries) {
                for (size_t i = 0; i < priv_count; i++) {
                    if (priv_entries[i]) {
                        nostr_nip51_list_add_entry(list, priv_entries[i]);
                    }
                }
                /* Only free the array, not the entries (ownership transferred) */
                free(priv_entries);
            }
        }
    }

    return list;
}
