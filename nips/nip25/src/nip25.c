#include "nostr/nip25/nip25.h"
#include "nostr-tag.h"
#include "nostr-kinds.h"
#include "nostr-utils.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

/* Helper: binary(32) -> hex(64) */
static void bin32_to_hex64(const unsigned char in[32], char out_hex[65]) {
    static const char *hex = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        out_hex[2*i]   = hex[(in[i] >> 4) & 0xF];
        out_hex[2*i+1] = hex[in[i] & 0xF];
    }
    out_hex[64] = '\0';
}

/* Helper: check if string is valid hex of given length */
static bool is_valid_hex(const char *s, size_t expected_len) {
    if (!s) return false;
    size_t len = strlen(s);
    if (len != expected_len) return false;
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

NostrReactionType nostr_nip25_get_reaction_type(const char *content) {
    if (!content || !*content) {
        return NOSTR_REACTION_LIKE; /* empty content treated as like */
    }

    if (strcmp(content, "+") == 0) {
        return NOSTR_REACTION_LIKE;
    }

    if (strcmp(content, "-") == 0) {
        return NOSTR_REACTION_DISLIKE;
    }

    /* Check for emoji (any non-empty content that isn't +/-) */
    return NOSTR_REACTION_EMOJI;
}

NostrEvent *nostr_nip25_create_reaction(
    const unsigned char reacted_event_id[32],
    const unsigned char *reacted_author_pubkey,
    int reacted_kind,
    const char *reaction_content,
    const char *relay_url
) {
    if (!reacted_event_id) return NULL;

    /* Convert binary to hex */
    char event_id_hex[65];
    bin32_to_hex64(reacted_event_id, event_id_hex);

    char author_pubkey_hex[65] = {0};
    if (reacted_author_pubkey) {
        bin32_to_hex64(reacted_author_pubkey, author_pubkey_hex);
    }

    return nostr_nip25_create_reaction_hex(
        event_id_hex,
        reacted_author_pubkey ? author_pubkey_hex : NULL,
        reacted_kind,
        reaction_content,
        relay_url
    );
}

NostrEvent *nostr_nip25_create_reaction_hex(
    const char *reacted_event_id_hex,
    const char *reacted_author_pubkey_hex,
    int reacted_kind,
    const char *reaction_content,
    const char *relay_url
) {
    /* Validate event ID */
    if (!is_valid_hex(reacted_event_id_hex, 64)) {
        return NULL;
    }

    /* Validate author pubkey if provided */
    if (reacted_author_pubkey_hex && !is_valid_hex(reacted_author_pubkey_hex, 64)) {
        return NULL;
    }

    /* Create event */
    NostrEvent *ev = nostr_event_new();
    if (!ev) return NULL;

    /* Set kind 7 (reaction) */
    nostr_event_set_kind(ev, NOSTR_KIND_REACTION);

    /* Set created_at to now */
    nostr_event_set_created_at(ev, (int64_t)time(NULL));

    /* Set content: default to "+" if not provided */
    const char *content = reaction_content && *reaction_content ? reaction_content : "+";
    nostr_event_set_content(ev, content);

    /* Build tags */
    NostrTags *tags = nostr_tags_new(0);
    if (!tags) {
        nostr_event_free(ev);
        return NULL;
    }

    /* e-tag: ["e", "<event-id>", "<relay-url>?"] */
    NostrTag *e_tag;
    if (relay_url && *relay_url) {
        e_tag = nostr_tag_new("e", reacted_event_id_hex, relay_url, NULL);
    } else {
        e_tag = nostr_tag_new("e", reacted_event_id_hex, NULL);
    }
    if (e_tag) {
        nostr_tags_append(tags, e_tag);
    }

    /* p-tag: ["p", "<pubkey>"] */
    if (reacted_author_pubkey_hex) {
        NostrTag *p_tag = nostr_tag_new("p", reacted_author_pubkey_hex, NULL);
        if (p_tag) {
            nostr_tags_append(tags, p_tag);
        }
    }

    /* k-tag: ["k", "<kind>"] - only if kind is known */
    if (reacted_kind >= 0) {
        char kind_str[16];
        snprintf(kind_str, sizeof(kind_str), "%d", reacted_kind);
        NostrTag *k_tag = nostr_tag_new("k", kind_str, NULL);
        if (k_tag) {
            nostr_tags_append(tags, k_tag);
        }
    }

    nostr_event_set_tags(ev, tags);

    return ev;
}

int nostr_nip25_parse_reaction(const NostrEvent *ev, NostrReaction *out) {
    if (!ev || !out) return -1;

    memset(out, 0, sizeof(*out));
    out->reacted_kind = -1;

    /* Check kind */
    int kind = nostr_event_get_kind(ev);
    if (kind != NOSTR_KIND_REACTION) {
        return -1;
    }

    /* Get content */
    const char *content = nostr_event_get_content(ev);
    if (content) {
        strncpy(out->content, content, sizeof(out->content) - 1);
        out->content[sizeof(out->content) - 1] = '\0';
    } else {
        out->content[0] = '+';
        out->content[1] = '\0';
    }
    out->type = nostr_nip25_get_reaction_type(out->content);

    /* Parse tags */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return 0; /* Valid reaction but no tags */

    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; ++i) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (!tag || nostr_tag_size(tag) < 2) continue;

        const char *key = nostr_tag_get(tag, 0);
        const char *val = nostr_tag_get(tag, 1);
        if (!key || !val) continue;

        if (strcmp(key, "e") == 0 && !out->has_event_id) {
            /* Last e-tag is the reacted event per NIP-25 */
            if (is_valid_hex(val, 64)) {
                if (nostr_hex2bin(out->event_id, val, 32)) {
                    out->has_event_id = true;
                }
            }
        } else if (strcmp(key, "p") == 0 && !out->has_author_pubkey) {
            if (is_valid_hex(val, 64)) {
                if (nostr_hex2bin(out->author_pubkey, val, 32)) {
                    out->has_author_pubkey = true;
                }
            }
        } else if (strcmp(key, "k") == 0 && out->reacted_kind < 0) {
            out->reacted_kind = atoi(val);
        }
    }

    return 0;
}

bool nostr_nip25_is_reaction(const NostrEvent *ev) {
    if (!ev) return false;
    return nostr_event_get_kind(ev) == NOSTR_KIND_REACTION;
}

bool nostr_nip25_is_like(const NostrEvent *ev) {
    if (!nostr_nip25_is_reaction(ev)) return false;

    const char *content = nostr_event_get_content(ev);
    /* Empty content or "+" is a like */
    return (!content || !*content || strcmp(content, "+") == 0);
}

bool nostr_nip25_is_dislike(const NostrEvent *ev) {
    if (!nostr_nip25_is_reaction(ev)) return false;

    const char *content = nostr_event_get_content(ev);
    return (content && strcmp(content, "-") == 0);
}

bool nostr_nip25_get_reacted_event_id(const NostrEvent *ev, unsigned char out_id[32]) {
    if (!ev || !out_id) return false;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return false;

    /* Find the last e-tag (per NIP-25, last e-tag is the reacted event) */
    const char *last_event_id = NULL;
    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; ++i) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (!tag || nostr_tag_size(tag) < 2) continue;

        const char *key = nostr_tag_get(tag, 0);
        if (key && strcmp(key, "e") == 0) {
            const char *val = nostr_tag_get(tag, 1);
            if (val && is_valid_hex(val, 64)) {
                last_event_id = val;
            }
        }
    }

    if (!last_event_id) return false;

    return nostr_hex2bin(out_id, last_event_id, 32);
}

char *nostr_nip25_get_reacted_event_id_hex(const NostrEvent *ev) {
    unsigned char id_bin[32];
    if (!nostr_nip25_get_reacted_event_id(ev, id_bin)) {
        return NULL;
    }

    return nostr_bin2hex(id_bin, 32);
}

int nostr_nip25_aggregate_reactions(
    const NostrEvent **reactions,
    size_t count,
    NostrReactionStats *out_stats
) {
    if (!out_stats) return -1;

    memset(out_stats, 0, sizeof(*out_stats));

    if (!reactions || count == 0) return 0;

    for (size_t i = 0; i < count; ++i) {
        const NostrEvent *ev = reactions[i];
        if (!nostr_nip25_is_reaction(ev)) continue;

        out_stats->total_count++;

        const char *content = nostr_event_get_content(ev);
        NostrReactionType type = nostr_nip25_get_reaction_type(content);

        switch (type) {
            case NOSTR_REACTION_LIKE:
                out_stats->like_count++;
                break;
            case NOSTR_REACTION_DISLIKE:
                out_stats->dislike_count++;
                break;
            case NOSTR_REACTION_EMOJI:
                out_stats->emoji_count++;
                break;
            default:
                break;
        }
    }

    return 0;
}
