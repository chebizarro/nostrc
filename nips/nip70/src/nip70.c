#include "nostr/nip70/nip70.h"
#include "nostr-tag.h"
#include <errno.h>
#include <string.h>

bool nostr_nip70_is_protected(const NostrEvent *ev) {
    if (!ev) return false;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return false;

    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t) continue;
        /* Protection tag is exactly ["-"] — one element, value is "-" */
        if (nostr_tag_size(t) == 1) {
            const char *k = nostr_tag_get(t, 0);
            if (k && strcmp(k, "-") == 0)
                return true;
        }
    }
    return false;
}

bool nostr_nip70_has_embedded_protected(const NostrEvent *ev) {
    if (!ev) return false;

    /* Only applies to reposts (kind 6) and generic reposts (kind 16) */
    int kind = nostr_event_get_kind(ev);
    if (kind != 6 && kind != 16) return false;

    const char *content = nostr_event_get_content(ev);
    if (!content || *content == '\0') return false;

    /*
     * Heuristic: look for ["-"] pattern inside the content's tags section.
     * We check that the pattern appears between "tags":[ and ]].
     * This matches the approach used by nostrlib (Go).
     */
    const char *tags_start = strstr(content, "\"tags\":[");
    if (!tags_start) return false;

    const char *tags_end = strstr(tags_start, "]]");
    if (!tags_end) return false;

    const char *prot = strstr(tags_start, "[\"-\"]");
    if (!prot) return false;

    return (prot > tags_start && prot < tags_end);
}

static NostrTag *clone_tag(const NostrTag *src) {
    if (!src) return NULL;
    size_t n = nostr_tag_size(src);
    const char *k0 = nostr_tag_get(src, 0);
    NostrTag *dst = nostr_tag_new(k0 ? k0 : "", NULL);
    for (size_t i = 1; i < n; ++i) {
        const char *v = nostr_tag_get(src, i);
        if (v) nostr_tag_append(dst, v);
    }
    return dst;
}

int nostr_nip70_add_protection(NostrEvent *ev) {
    if (!ev) return -EINVAL;

    /* Idempotent: if already protected, do nothing */
    if (nostr_nip70_is_protected(ev)) return 0;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    NostrTags *updated = nostr_tags_new(0);
    if (!updated) return -ENOMEM;

    /* Copy existing tags */
    if (tags) {
        size_t n = nostr_tags_size(tags);
        for (size_t i = 0; i < n; ++i) {
            NostrTag *t = nostr_tags_get(tags, i);
            if (!t) continue;
            NostrTag *dup = clone_tag(t);
            if (dup) nostr_tags_append(updated, dup);
        }
    }

    /* Add ["-"] protection tag */
    NostrTag *prot_tag = nostr_tag_new("-", NULL);
    if (!prot_tag) { nostr_tags_free(updated); return -ENOMEM; }
    nostr_tags_append(updated, prot_tag);

    nostr_event_set_tags(ev, updated);
    return 0;
}

int nostr_nip70_remove_protection(NostrEvent *ev) {
    if (!ev) return -EINVAL;

    /* If not protected, nothing to do */
    if (!nostr_nip70_is_protected(ev)) return 0;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    NostrTags *filtered = nostr_tags_new(0);
    if (!filtered) return -ENOMEM;

    if (tags) {
        size_t n = nostr_tags_size(tags);
        for (size_t i = 0; i < n; ++i) {
            NostrTag *t = nostr_tags_get(tags, i);
            if (!t) continue;
            /* Skip protection tags: exactly ["-"] */
            if (nostr_tag_size(t) == 1) {
                const char *k = nostr_tag_get(t, 0);
                if (k && strcmp(k, "-") == 0)
                    continue;
            }
            NostrTag *dup = clone_tag(t);
            if (dup) nostr_tags_append(filtered, dup);
        }
    }

    nostr_event_set_tags(ev, filtered);
    return 0;
}

bool nostr_nip70_can_rebroadcast(const NostrEvent *ev) {
    if (!ev) return false;
    return !nostr_nip70_is_protected(ev);
}
