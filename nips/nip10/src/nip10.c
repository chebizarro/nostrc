#include "nostr/nip10/nip10.h"
#include "nostr-tag.h"
#include "nostr-utils.h"
#include <string.h>

/* local helper: binary(32) -> hex(64) */
static void bin32_to_hex64(const unsigned char in[32], char out_hex[65]) {
    static const char *hex = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        out_hex[2*i]   = hex[(in[i] >> 4) & 0xF];
        out_hex[2*i+1] = hex[in[i] & 0xF];
    }
    out_hex[64] = '\0';
}

int nostr_nip10_add_marked_e_tag(NostrEvent *ev,
                                 const unsigned char event_id[32],
                                 const char *relay_opt,
                                 NostrEMarker marker,
                                 const unsigned char *author_pk_opt) {
    if (!ev || !event_id) return -1;

    char id_hex[65];
    bin32_to_hex64(event_id, id_hex);

    NostrTag *etag = NULL;
    if (relay_opt && *relay_opt) {
        switch (marker) {
            case NOSTR_E_MARK_ROOT:
                etag = nostr_tag_new("e", id_hex, relay_opt, "root", NULL);
                break;
            case NOSTR_E_MARK_REPLY:
                etag = nostr_tag_new("e", id_hex, relay_opt, "reply", NULL);
                break;
            default:
                etag = nostr_tag_new("e", id_hex, relay_opt, NULL);
                break;
        }
    } else {
        switch (marker) {
            case NOSTR_E_MARK_ROOT:
                etag = nostr_tag_new("e", id_hex, NULL);
                if (etag) { nostr_tag_append(etag, ""); nostr_tag_append(etag, "root"); }
                break;
            case NOSTR_E_MARK_REPLY:
                etag = nostr_tag_new("e", id_hex, NULL);
                if (etag) { nostr_tag_append(etag, ""); nostr_tag_append(etag, "reply"); }
                break;
            default:
                etag = nostr_tag_new("e", id_hex, NULL);
                break;
        }
    }

    if (!etag) return -1;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) {
        tags = nostr_tags_new(1, etag);
        if (!tags) return -1;
        nostr_event_set_tags(ev, tags);
    } else {
        NostrTags *updated = nostr_tags_append_unique(tags, etag);
        if (updated != tags) {
            nostr_event_set_tags(ev, updated);
        }
    }

    (void)author_pk_opt; // reserved for future use
    return 0;
}

int nostr_nip10_ensure_p_participants(NostrEvent *reply_ev, const NostrEvent *parent_ev) {
    if (!reply_ev || !parent_ev) return -1;

    NostrTags *reply_tags = (NostrTags *)nostr_event_get_tags(reply_ev);
    if (!reply_tags) {
        reply_tags = nostr_tags_new(0);
        if (!reply_tags) return -1;
        nostr_event_set_tags(reply_ev, reply_tags);
    }

    const char *parent_pub_hex = nostr_event_get_pubkey(parent_ev);
    if (parent_pub_hex && *parent_pub_hex) {
        NostrTag *ptag = nostr_tag_new("p", parent_pub_hex, NULL);
        if (!ptag) return -1;
        NostrTags *updated = nostr_tags_append_unique(reply_tags, ptag);
        if (updated != reply_tags) {
            nostr_event_set_tags(reply_ev, updated);
            reply_tags = updated;
        }
    }

    NostrTags *parent_tags = (NostrTags *)nostr_event_get_tags(parent_ev);
    if (parent_tags) {
        size_t n = nostr_tags_size(parent_tags);
        for (size_t i = 0; i < n; ++i) {
            NostrTag *t = nostr_tags_get(parent_tags, i);
            if (!t || nostr_tag_size(t) == 0) continue;
            const char *k = nostr_tag_get(t, 0);
            if (!k || strcmp(k, "p") != 0) continue;
            const char *val = (nostr_tag_size(t) >= 2) ? nostr_tag_get(t, 1) : NULL;
            const char *relay = (nostr_tag_size(t) >= 3) ? nostr_tag_get(t, 2) : NULL;
            if (!val || !*val) continue;
            NostrTag *ptag = relay && *relay ? nostr_tag_new("p", val, relay, NULL)
                                             : nostr_tag_new("p", val, NULL);
            if (!ptag) return -1;
            NostrTags *updated = nostr_tags_append_unique(reply_tags, ptag);
            if (updated != reply_tags) {
                nostr_event_set_tags(reply_ev, updated);
                reply_tags = updated;
            }
        }
    }

    return 0;
}

static NostrTag* get_thread_root(NostrTags* tags) {
    if (!tags) return NULL;
    NostrTag* first_e = NULL;
    for (size_t i = 0; i < tags->count; i++) {
        NostrTag* tag = tags->data[i];
        if (!tag || nostr_tag_size(tag) == 0) continue;
        const char *key = nostr_tag_get(tag, 0);
        if (!key || strcmp(key, "e") != 0) continue;
        if (!first_e) first_e = tag;
        if (nostr_tag_size(tag) >= 4) {
            const char *marker = nostr_tag_get(tag, 3);
            if (marker && strcmp(marker, "root") == 0) {
                return tag;
            }
        }
    }
    return first_e;
}

static NostrTag* get_immediate_reply(NostrTags* tags) {
    if (!tags) return NULL;
    NostrTag* root = NULL;
    NostrTag* last_e = NULL;

    for (size_t i = 0; i < tags->count; i++) {
        NostrTag* tag = tags->data[i];
        if (!tag || nostr_tag_size(tag) < 2) continue;

        const char *key = nostr_tag_get(tag, 0);
        if (!key) continue;
        if (strcmp(key, "e") != 0 && strcmp(key, "a") != 0) continue;

        if (nostr_tag_size(tag) >= 4) {
            const char *marker = nostr_tag_get(tag, 3);
            if (marker) {
                if (strcmp(marker, "reply") == 0) {
                    return tag;
                }
                if (strcmp(marker, "root") == 0) {
                    root = tag;
                    continue;
                }
                if (strcmp(marker, "mention") == 0) {
                    continue;
                }
            }
        }

        if (strcmp(key, "e") == 0) {
            last_e = tag;
        }
    }

    if (root != NULL) return root;
    return last_e;
}

int nostr_nip10_get_thread(const NostrEvent *ev, NostrThreadContext *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!ev) return 0;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags || tags->count == 0) return 0;

    // Prefer explicit markers per NIP-10
    NostrTag *root_tag = get_thread_root(tags);
    NostrTag *reply_tag = get_immediate_reply(tags);

    if (root_tag && nostr_tag_size(root_tag) >= 2) {
        const char *rid = nostr_tag_get(root_tag, 1);
        if (rid && strlen(rid) == 64) { // hex id
            // store as hex in out->root_id (binary would require decode util)
            // Convert hex to bin using nostr_hex2bin
            if (nostr_hex2bin(out->root_id, rid, sizeof(out->root_id))) {
                out->has_root = true;
            }
        }
    }

    if (reply_tag && nostr_tag_size(reply_tag) >= 2) {
        const char *rid = nostr_tag_get(reply_tag, 1);
        if (rid && strlen(rid) == 64) {
            if (nostr_hex2bin(out->reply_id, rid, sizeof(out->reply_id))) {
                out->has_reply = true;
            }
        }
    }

    return 0;
}
