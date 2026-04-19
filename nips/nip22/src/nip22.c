#include "nostr/nip22/nip22.h"
#include "nostr-tag.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

bool nostr_nip22_is_comment(const NostrEvent *ev) {
    if (!ev) return false;
    return nostr_event_get_kind(ev) == NOSTR_KIND_COMMENT;
}

/*
 * Internal: scan tags for the first tag whose key matches one of
 * the given single-character keys. Returns a Ref with the tag value
 * and optional relay hint.
 *
 * NIP-22 convention:
 *   Uppercase E/A/I → thread root
 *   Lowercase e/a/i → immediate parent
 */
static NostrNip22Ref find_ref(const NostrEvent *ev,
                               const char *event_key,
                               const char *addr_key,
                               const char *ext_key) {
    NostrNip22Ref ref = { .type = NOSTR_NIP22_REF_NONE, .value = NULL, .relay = NULL };

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return ref;

    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;

        const char *k = nostr_tag_get(t, 0);
        if (!k) continue;

        NostrNip22RefType type = NOSTR_NIP22_REF_NONE;
        if (strcmp(k, event_key) == 0)
            type = NOSTR_NIP22_REF_EVENT;
        else if (strcmp(k, addr_key) == 0)
            type = NOSTR_NIP22_REF_ADDR;
        else if (strcmp(k, ext_key) == 0)
            type = NOSTR_NIP22_REF_EXTERNAL;
        else
            continue;

        ref.type = type;
        ref.value = nostr_tag_get(t, 1);
        ref.relay = (nostr_tag_size(t) >= 3) ? nostr_tag_get(t, 2) : NULL;
        /* Return empty relay as NULL for convenience */
        if (ref.relay && ref.relay[0] == '\0') ref.relay = NULL;
        return ref;
    }
    return ref;
}

NostrNip22Ref nostr_nip22_get_thread_root(const NostrEvent *ev) {
    NostrNip22Ref none = { .type = NOSTR_NIP22_REF_NONE, .value = NULL, .relay = NULL };
    if (!ev) return none;
    return find_ref(ev, "E", "A", "I");
}

NostrNip22Ref nostr_nip22_get_immediate_parent(const NostrEvent *ev) {
    NostrNip22Ref none = { .type = NOSTR_NIP22_REF_NONE, .value = NULL, .relay = NULL };
    if (!ev) return none;
    return find_ref(ev, "e", "a", "i");
}

/*
 * Internal: extract a kind number from a "K" or "k" tag.
 */
static int get_kind_tag(const NostrEvent *ev, const char *tag_key, int *out_kind) {
    if (!ev || !out_kind) return -EINVAL;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return -ENOENT;

    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;

        const char *k = nostr_tag_get(t, 0);
        if (!k || strcmp(k, tag_key) != 0) continue;

        const char *v = nostr_tag_get(t, 1);
        if (!v || *v == '\0') continue;

        char *endptr = NULL;
        long val = strtol(v, &endptr, 10);
        if (endptr == v || *endptr != '\0') continue;
        if (val < 0 || val > 65535) continue;

        *out_kind = (int)val;
        return 0;
    }
    return -ENOENT;
}

int nostr_nip22_get_root_kind(const NostrEvent *ev, int *out_kind) {
    return get_kind_tag(ev, "K", out_kind);
}

int nostr_nip22_get_parent_kind(const NostrEvent *ev, int *out_kind) {
    return get_kind_tag(ev, "k", out_kind);
}

int nostr_nip22_create_comment(NostrEvent *ev, const char *content,
                                const char *root_id, const char *root_relay,
                                int root_kind,
                                const char *parent_id, const char *parent_relay,
                                int parent_kind) {
    if (!ev) return -EINVAL;

    /* Set kind and content */
    nostr_event_set_kind(ev, NOSTR_KIND_COMMENT);
    if (content)
        nostr_event_set_content(ev, content);

    /* Build tags */
    NostrTags *tags = nostr_tags_new(0);
    if (!tags) return -ENOMEM;

    /* Root event reference: ["E", id, relay] */
    if (root_id) {
        NostrTag *tag;
        if (root_relay)
            tag = nostr_tag_new("E", root_id, root_relay, NULL);
        else
            tag = nostr_tag_new("E", root_id, NULL);
        if (!tag) { nostr_tags_free(tags); return -ENOMEM; }
        nostr_tags_append(tags, tag);
    }

    /* Immediate parent: ["e", id, relay] */
    if (parent_id) {
        NostrTag *tag;
        if (parent_relay)
            tag = nostr_tag_new("e", parent_id, parent_relay, NULL);
        else
            tag = nostr_tag_new("e", parent_id, NULL);
        if (!tag) { nostr_tags_free(tags); return -ENOMEM; }
        nostr_tags_append(tags, tag);
    }

    /* Root kind: ["K", "kind-str"] */
    if (root_kind >= 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", root_kind);
        NostrTag *tag = nostr_tag_new("K", buf, NULL);
        if (!tag) { nostr_tags_free(tags); return -ENOMEM; }
        nostr_tags_append(tags, tag);
    }

    /* Parent kind: ["k", "kind-str"] */
    if (parent_kind >= 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", parent_kind);
        NostrTag *tag = nostr_tag_new("k", buf, NULL);
        if (!tag) { nostr_tags_free(tags); return -ENOMEM; }
        nostr_tags_append(tags, tag);
    }

    nostr_event_set_tags(ev, tags);
    return 0;
}
