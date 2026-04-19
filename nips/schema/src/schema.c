#include "nostr/schema/schema.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Type validators ---- */

static bool is_hex_char(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

bool nostr_schema_is_valid_hex(const char *str) {
    if (!str || !*str) return false;
    size_t len = strlen(str);
    if (len % 2 != 0) return false;
    for (size_t i = 0; i < len; ++i) {
        if (!is_hex_char(str[i])) return false;
    }
    return true;
}

bool nostr_schema_is_valid_id(const char *str) {
    if (!str) return false;
    if (strlen(str) != 64) return false;
    return nostr_schema_is_valid_hex(str);
}

bool nostr_schema_is_valid_pubkey(const char *str) {
    return nostr_schema_is_valid_id(str); /* Same format: 64 hex chars */
}

bool nostr_schema_is_valid_relay_url(const char *str) {
    if (!str || !*str) return false;

    if (strncmp(str, "wss://", 6) == 0) return strlen(str) > 6;
    if (strncmp(str, "ws://", 5) == 0) return strlen(str) > 5;
    return false;
}

bool nostr_schema_is_valid_kind_str(const char *str) {
    if (!str || !*str) return false;
    for (const char *p = str; *p; ++p) {
        if (!isdigit((unsigned char)*p)) return false;
    }
    return true;
}

bool nostr_schema_is_valid_timestamp(const char *str) {
    return nostr_schema_is_valid_kind_str(str); /* Same format: unsigned integer */
}

bool nostr_schema_is_trimmed(const char *str) {
    if (!str) return true;
    if (!*str) return true;

    if (isspace((unsigned char)str[0])) return false;
    size_t len = strlen(str);
    if (isspace((unsigned char)str[len - 1])) return false;
    return true;
}

bool nostr_schema_is_valid_json(const char *str) {
    if (!str || !*str) return false;

    /* Must start with { or [ */
    const char *p = str;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p != '{' && *p != '[') return false;

    /* Simple depth-tracking validator */
    int depth = 0;
    bool in_string = false;

    for (p = str; *p; ++p) {
        if (in_string) {
            if (*p == '\\' && *(p + 1)) {
                ++p; /* skip escaped char */
            } else if (*p == '"') {
                in_string = false;
            }
            continue;
        }

        switch (*p) {
            case '"': in_string = true; break;
            case '{': case '[': ++depth; break;
            case '}': case ']':
                --depth;
                if (depth < 0) return false;
                break;
        }
    }

    return depth == 0 && !in_string;
}

bool nostr_schema_is_valid_addr(const char *str) {
    if (!str || !*str) return false;

    /* Format: <kind>:<pubkey>:<d-tag> */
    const char *first_colon = strchr(str, ':');
    if (!first_colon) return false;

    const char *second_colon = strchr(first_colon + 1, ':');
    if (!second_colon) return false;

    /* Kind part must be digits */
    for (const char *p = str; p < first_colon; ++p) {
        if (!isdigit((unsigned char)*p)) return false;
    }
    if (first_colon == str) return false; /* empty kind */

    /* Pubkey part must be 64 hex chars */
    size_t pk_len = (size_t)(second_colon - first_colon - 1);
    if (pk_len != 64) return false;
    for (const char *p = first_colon + 1; p < second_colon; ++p) {
        if (!is_hex_char(*p)) return false;
    }

    /* d-tag can be anything (even empty) */
    return true;
}

/* ---- Kind classification ---- */

bool nostr_schema_is_addressable(int kind) {
    return (kind >= 30000 && kind < 40000);
}

bool nostr_schema_is_replaceable(int kind) {
    return kind == 0 || kind == 3 ||
           (kind >= 10000 && kind < 20000);
}

bool nostr_schema_is_ephemeral(int kind) {
    return (kind >= 20000 && kind < 30000);
}

/* ---- Type dispatch ---- */

bool nostr_schema_validate_type(const char *type_name, const char *value) {
    if (!type_name || !value) return false;

    if (strcmp(type_name, "id") == 0)        return nostr_schema_is_valid_id(value);
    if (strcmp(type_name, "pubkey") == 0)     return nostr_schema_is_valid_pubkey(value);
    if (strcmp(type_name, "relay") == 0)      return nostr_schema_is_valid_relay_url(value);
    if (strcmp(type_name, "kind") == 0)       return nostr_schema_is_valid_kind_str(value);
    if (strcmp(type_name, "timestamp") == 0)  return nostr_schema_is_valid_timestamp(value);
    if (strcmp(type_name, "hex") == 0)        return nostr_schema_is_valid_hex(value);
    if (strcmp(type_name, "json") == 0)       return nostr_schema_is_valid_json(value);
    if (strcmp(type_name, "addr") == 0)       return nostr_schema_is_valid_addr(value);
    if (strcmp(type_name, "free") == 0)       return true; /* accepts anything */

    return false; /* unknown type */
}

/* ---- Event validation ---- */

static NostrSchemaResult make_ok(void) {
    NostrSchemaResult r = { .valid = true, .tag_index = -1, .item_index = -1 };
    r.error[0] = '\0';
    return r;
}

static NostrSchemaResult make_err(const char *msg, int tag_idx, int item_idx) {
    NostrSchemaResult r = { .valid = false, .tag_index = tag_idx, .item_index = item_idx };
    snprintf(r.error, sizeof(r.error), "%s", msg);
    return r;
}

static NostrSchemaResult make_err_fmt(int tag_idx, int item_idx,
                                       const char *fmt, ...) {
    NostrSchemaResult r = { .valid = false, .tag_index = tag_idx, .item_index = item_idx };
    va_list args;
    va_start(args, fmt);
    vsnprintf(r.error, sizeof(r.error), fmt, args);
    va_end(args);
    return r;
}

NostrSchemaResult nostr_schema_validate_event(const NostrEvent *ev) {
    if (!ev) return make_err("null event", -1, -1);

    int kind = nostr_event_get_kind(ev);
    const char *content = nostr_event_get_content(ev);

    /* Check content is trimmed */
    if (content && !nostr_schema_is_trimmed(content)) {
        return make_err("content has dangling whitespace", -1, -1);
    }

    /* Kind 0: content must be valid JSON */
    if (kind == 0 && content) {
        if (!nostr_schema_is_valid_json(content)) {
            return make_err("kind 0 content must be valid JSON", -1, -1);
        }
    }

    const NostrTags *tags = nostr_event_get_tags(ev);
    size_t n_tags = nostr_tags_size(tags);

    bool has_d_tag = false;

    for (size_t i = 0; i < n_tags; ++i) {
        const NostrTag *t = nostr_tags_get(tags, i);
        size_t t_size = nostr_tag_size(t);

        /* No empty tags */
        if (t_size == 0) {
            return make_err_fmt((int)i, -1, "tag[%zu]: empty tag", i);
        }

        const char *key = nostr_tag_get(t, 0);
        if (!key || !*key) {
            return make_err_fmt((int)i, 0, "tag[%zu]: empty tag name", i);
        }

        /* Track "d" tag */
        if (strcmp(key, "d") == 0) {
            has_d_tag = true;
            /* "d" tag in non-addressable event is an error */
            if (!nostr_schema_is_addressable(kind)) {
                return make_err_fmt((int)i, -1,
                    "tag[%zu]: 'd' tag in non-addressable kind %d", i, kind);
            }
        }

        /* Validate "e" tag values as event IDs */
        if (strcmp(key, "e") == 0 && t_size >= 2) {
            const char *val = nostr_tag_get(t, 1);
            if (val && *val && !nostr_schema_is_valid_id(val)) {
                return make_err_fmt((int)i, 1,
                    "tag[%zu]: invalid event ID in 'e' tag", i);
            }
            /* Check relay hint if present */
            if (t_size >= 3) {
                const char *relay = nostr_tag_get(t, 2);
                if (relay && *relay && !nostr_schema_is_valid_relay_url(relay)) {
                    return make_err_fmt((int)i, 2,
                        "tag[%zu]: invalid relay URL in 'e' tag", i);
                }
            }
        }

        /* Validate "p" tag values as pubkeys */
        if (strcmp(key, "p") == 0 && t_size >= 2) {
            const char *val = nostr_tag_get(t, 1);
            if (val && *val && !nostr_schema_is_valid_pubkey(val)) {
                return make_err_fmt((int)i, 1,
                    "tag[%zu]: invalid pubkey in 'p' tag", i);
            }
        }

        /* Validate "a" tag values as addr references */
        if (strcmp(key, "a") == 0 && t_size >= 2) {
            const char *val = nostr_tag_get(t, 1);
            if (val && *val && !nostr_schema_is_valid_addr(val)) {
                return make_err_fmt((int)i, 1,
                    "tag[%zu]: invalid addr reference in 'a' tag", i);
            }
        }
    }

    /* Addressable events must have a "d" tag */
    if (nostr_schema_is_addressable(kind) && !has_d_tag) {
        return make_err("addressable event missing 'd' tag", -1, -1);
    }

    return make_ok();
}
