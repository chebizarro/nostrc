#include "nostr/nip18/nip18.h"
#include "nostr-tag.h"
#include "nostr-json.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#if defined(NOSTR_DEBUG) || defined(NOSTR_NIP18_DEBUG)
#include <stdio.h>
#define NIP18_DEBUGF(...) fprintf(stderr, __VA_ARGS__)
#else
#define NIP18_DEBUGF(...) do { } while (0)
#endif

/* Helper: binary(32) -> hex(64) */
static char *hex_from_32(const unsigned char bin[32]) {
    static const char *hex = "0123456789abcdef";
    char *out = (char *)malloc(65);
    if (!out) return NULL;
    for (size_t i = 0; i < 32; ++i) {
        out[2*i]   = hex[(bin[i] >> 4) & 0xF];
        out[2*i+1] = hex[bin[i] & 0xF];
    }
    out[64] = '\0';
    return out;
}

/* Helper: hex(64) -> binary(32) */
static bool hex_to_bin_32(const char *hex, unsigned char out[32]) {
    if (!hex || strlen(hex) != 64) return false;
    for (size_t i = 0; i < 32; ++i) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return false;
        out[i] = (unsigned char)byte;
    }
    return true;
}

void nostr_nip18_repost_info_clear(NostrRepostInfo *info) {
    if (!info) return;
    if (info->relay_hint) {
        free(info->relay_hint);
        info->relay_hint = NULL;
    }
    if (info->embedded_json) {
        free(info->embedded_json);
        info->embedded_json = NULL;
    }
    info->has_repost_event = false;
    info->has_repost_pubkey = false;
    info->repost_kind = 0;
    memset(info->repost_event_id, 0, 32);
    memset(info->repost_pubkey, 0, 32);
}

void nostr_nip18_quote_info_clear(NostrQuoteInfo *info) {
    if (!info) return;
    if (info->relay_hint) {
        free(info->relay_hint);
        info->relay_hint = NULL;
    }
    info->has_quoted_event = false;
    info->has_quoted_pubkey = false;
    memset(info->quoted_event_id, 0, 32);
    memset(info->quoted_pubkey, 0, 32);
}

/* Helper: Create repost event with given kind */
static NostrEvent *create_repost_internal(int kind,
                                           const unsigned char event_id[32],
                                           const unsigned char author_pubkey[32],
                                           int reposted_kind,
                                           const char *relay_hint,
                                           const char *event_json) {
    if (!event_id || !author_pubkey) return NULL;

    NostrEvent *ev = nostr_event_new();
    if (!ev) return NULL;

    nostr_event_set_kind(ev, kind);
    nostr_event_set_created_at(ev, (int64_t)time(NULL));

    /* Set content: either empty or the JSON of the reposted event */
    nostr_event_set_content(ev, event_json ? event_json : "");

    /* Create tags */
    NostrTags *tags = nostr_tags_new(0);
    if (!tags) {
        nostr_event_free(ev);
        return NULL;
    }

    /* Add e-tag with relay hint: ["e", <id>, <relay?>] */
    char *id_hex = hex_from_32(event_id);
    if (!id_hex) {
        nostr_tags_free(tags);
        nostr_event_free(ev);
        return NULL;
    }

    NostrTag *e_tag;
    if (relay_hint && *relay_hint) {
        e_tag = nostr_tag_new("e", id_hex, relay_hint, NULL);
    } else {
        e_tag = nostr_tag_new("e", id_hex, NULL);
    }
    free(id_hex);

    if (!e_tag) {
        nostr_tags_free(tags);
        nostr_event_free(ev);
        return NULL;
    }
    nostr_tags_append(tags, e_tag);

    /* Add p-tag: ["p", <pubkey>] */
    char *pk_hex = hex_from_32(author_pubkey);
    if (!pk_hex) {
        nostr_tags_free(tags);
        nostr_event_free(ev);
        return NULL;
    }

    NostrTag *p_tag = nostr_tag_new("p", pk_hex, NULL);
    free(pk_hex);

    if (!p_tag) {
        nostr_tags_free(tags);
        nostr_event_free(ev);
        return NULL;
    }
    nostr_tags_append(tags, p_tag);

    /* For kind 16 (generic repost), add k-tag with the reposted event's kind */
    if (kind == NOSTR_KIND_GENERIC_REPOST) {
        char kind_str[12];
        snprintf(kind_str, sizeof(kind_str), "%d", reposted_kind);
        NostrTag *k_tag = nostr_tag_new("k", kind_str, NULL);
        if (k_tag) {
            nostr_tags_append(tags, k_tag);
        }
    }

    nostr_event_set_tags(ev, tags);
    return ev;
}

NostrEvent *nostr_nip18_create_repost(const NostrEvent *reposted_event,
                                       const char *relay_hint,
                                       bool include_json) {
    if (!reposted_event) return NULL;

    int kind = nostr_event_get_kind(reposted_event);
    if (kind != 1) {
        NIP18_DEBUGF("[nip18] create_repost: expected kind 1, got %d\n", kind);
        return NULL;
    }

    /* Get event ID as binary */
    char *id_hex = nostr_event_get_id((NostrEvent *)reposted_event);
    if (!id_hex) return NULL;

    unsigned char event_id[32];
    if (!hex_to_bin_32(id_hex, event_id)) {
        free(id_hex);
        return NULL;
    }
    free(id_hex);

    /* Get author pubkey as binary */
    const char *pk_hex = nostr_event_get_pubkey(reposted_event);
    if (!pk_hex) return NULL;

    unsigned char author_pk[32];
    if (!hex_to_bin_32(pk_hex, author_pk)) {
        return NULL;
    }

    /* Get JSON if needed */
    char *json = NULL;
    if (include_json) {
        json = nostr_event_serialize_compact(reposted_event);
    }

    NostrEvent *repost = create_repost_internal(NOSTR_KIND_REPOST,
                                                 event_id, author_pk,
                                                 1, relay_hint, json);
    if (json) free(json);
    return repost;
}

NostrEvent *nostr_nip18_create_repost_from_id(const unsigned char event_id[32],
                                               const unsigned char author_pubkey[32],
                                               const char *relay_hint,
                                               const char *event_json) {
    return create_repost_internal(NOSTR_KIND_REPOST,
                                   event_id, author_pubkey,
                                   1, relay_hint, event_json);
}

NostrEvent *nostr_nip18_create_generic_repost(const NostrEvent *reposted_event,
                                               const char *relay_hint,
                                               bool include_json) {
    if (!reposted_event) return NULL;

    int kind = nostr_event_get_kind(reposted_event);

    /* Get event ID as binary */
    char *id_hex = nostr_event_get_id((NostrEvent *)reposted_event);
    if (!id_hex) return NULL;

    unsigned char event_id[32];
    if (!hex_to_bin_32(id_hex, event_id)) {
        free(id_hex);
        return NULL;
    }
    free(id_hex);

    /* Get author pubkey as binary */
    const char *pk_hex = nostr_event_get_pubkey(reposted_event);
    if (!pk_hex) return NULL;

    unsigned char author_pk[32];
    if (!hex_to_bin_32(pk_hex, author_pk)) {
        return NULL;
    }

    /* Get JSON if needed */
    char *json = NULL;
    if (include_json) {
        json = nostr_event_serialize_compact(reposted_event);
    }

    NostrEvent *repost = create_repost_internal(NOSTR_KIND_GENERIC_REPOST,
                                                 event_id, author_pk,
                                                 kind, relay_hint, json);
    if (json) free(json);
    return repost;
}

NostrEvent *nostr_nip18_create_generic_repost_from_id(const unsigned char event_id[32],
                                                       const unsigned char author_pubkey[32],
                                                       int reposted_kind,
                                                       const char *relay_hint,
                                                       const char *event_json) {
    return create_repost_internal(NOSTR_KIND_GENERIC_REPOST,
                                   event_id, author_pubkey,
                                   reposted_kind, relay_hint, event_json);
}

int nostr_nip18_parse_repost(const NostrEvent *ev, NostrRepostInfo *out) {
    if (!out) return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!ev) return -EINVAL;

    int kind = nostr_event_get_kind(ev);
    if (kind != NOSTR_KIND_REPOST && kind != NOSTR_KIND_GENERIC_REPOST) {
        return -EINVAL;
    }

    /* Default repost kind based on event kind */
    out->repost_kind = (kind == NOSTR_KIND_REPOST) ? 1 : 0;

    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return 0;

    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;

        const char *key = nostr_tag_get(t, 0);
        if (!key) continue;

        /* Parse e-tag: ["e", <id>, <relay?>] */
        if (strcmp(key, "e") == 0 && !out->has_repost_event) {
            const char *id_hex = nostr_tag_get(t, 1);
            if (id_hex && hex_to_bin_32(id_hex, out->repost_event_id)) {
                out->has_repost_event = true;
                /* Get relay hint if present */
                if (nostr_tag_size(t) >= 3) {
                    const char *relay = nostr_tag_get(t, 2);
                    if (relay && *relay) {
                        out->relay_hint = strdup(relay);
                    }
                }
            }
        }

        /* Parse p-tag: ["p", <pubkey>] */
        if (strcmp(key, "p") == 0 && !out->has_repost_pubkey) {
            const char *pk_hex = nostr_tag_get(t, 1);
            if (pk_hex && hex_to_bin_32(pk_hex, out->repost_pubkey)) {
                out->has_repost_pubkey = true;
            }
        }

        /* Parse k-tag: ["k", <kind>] (for generic reposts) */
        if (strcmp(key, "k") == 0 && kind == NOSTR_KIND_GENERIC_REPOST) {
            const char *kind_str = nostr_tag_get(t, 1);
            if (kind_str) {
                out->repost_kind = atoi(kind_str);
            }
        }
    }

    /* Check if content contains embedded JSON */
    const char *content = nostr_event_get_content(ev);
    if (content && *content && content[0] == '{') {
        out->embedded_json = strdup(content);
    }

    return 0;
}

bool nostr_nip18_is_repost(const NostrEvent *ev) {
    if (!ev) return false;
    int kind = nostr_event_get_kind(ev);
    return kind == NOSTR_KIND_REPOST || kind == NOSTR_KIND_GENERIC_REPOST;
}

bool nostr_nip18_is_note_repost(const NostrEvent *ev) {
    if (!ev) return false;
    return nostr_event_get_kind(ev) == NOSTR_KIND_REPOST;
}

bool nostr_nip18_is_generic_repost(const NostrEvent *ev) {
    if (!ev) return false;
    return nostr_event_get_kind(ev) == NOSTR_KIND_GENERIC_REPOST;
}

int nostr_nip18_add_q_tag(NostrEvent *ev,
                          const unsigned char quoted_event_id[32],
                          const char *relay_hint,
                          const unsigned char *author_pubkey) {
    if (!ev || !quoted_event_id) return -EINVAL;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) {
        tags = nostr_tags_new(0);
        if (!tags) return -ENOMEM;
        nostr_event_set_tags(ev, tags);
    }

    char *id_hex = hex_from_32(quoted_event_id);
    if (!id_hex) return -ENOMEM;

    NostrTag *q_tag = NULL;

    if (relay_hint && *relay_hint && author_pubkey) {
        char *pk_hex = hex_from_32(author_pubkey);
        if (!pk_hex) {
            free(id_hex);
            return -ENOMEM;
        }
        q_tag = nostr_tag_new("q", id_hex, relay_hint, pk_hex, NULL);
        free(pk_hex);
    } else if (relay_hint && *relay_hint) {
        q_tag = nostr_tag_new("q", id_hex, relay_hint, NULL);
    } else if (author_pubkey) {
        char *pk_hex = hex_from_32(author_pubkey);
        if (!pk_hex) {
            free(id_hex);
            return -ENOMEM;
        }
        /* q-tag with empty relay and pubkey */
        q_tag = nostr_tag_new("q", id_hex, "", pk_hex, NULL);
        free(pk_hex);
    } else {
        q_tag = nostr_tag_new("q", id_hex, NULL);
    }

    free(id_hex);

    if (!q_tag) return -ENOMEM;
    nostr_tags_append(tags, q_tag);
    return 0;
}

int nostr_nip18_get_quote(const NostrEvent *ev, NostrQuoteInfo *out) {
    if (!out) return -EINVAL;
    memset(out, 0, sizeof(*out));
    if (!ev) return -EINVAL;

    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return -ENOENT;

    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;

        const char *key = nostr_tag_get(t, 0);
        if (!key || strcmp(key, "q") != 0) continue;

        /* Found q-tag: ["q", <id>, <relay?>, <pubkey?>] */
        const char *id_hex = nostr_tag_get(t, 1);
        if (id_hex && hex_to_bin_32(id_hex, out->quoted_event_id)) {
            out->has_quoted_event = true;

            /* Get relay hint if present */
            if (nostr_tag_size(t) >= 3) {
                const char *relay = nostr_tag_get(t, 2);
                if (relay && *relay) {
                    out->relay_hint = strdup(relay);
                }
            }

            /* Get pubkey if present */
            if (nostr_tag_size(t) >= 4) {
                const char *pk_hex = nostr_tag_get(t, 3);
                if (pk_hex && hex_to_bin_32(pk_hex, out->quoted_pubkey)) {
                    out->has_quoted_pubkey = true;
                }
            }

            return 0;
        }
    }

    return -ENOENT;
}

bool nostr_nip18_has_quote(const NostrEvent *ev) {
    if (!ev) return false;

    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return false;

    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;

        const char *key = nostr_tag_get(t, 0);
        if (key && strcmp(key, "q") == 0) {
            return true;
        }
    }

    return false;
}
