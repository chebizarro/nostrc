#include "nostr/nip75/nip75.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int nostr_nip75_parse(const NostrEvent *ev, NostrNip75Goal *out) {
    if (!ev || !out) return -EINVAL;
    memset(out, 0, sizeof(*out));

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return -ENOENT;

    bool found_amount = false;
    size_t n = nostr_tags_size(tags);

    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;

        const char *k = nostr_tag_get(t, 0);
        const char *v = nostr_tag_get(t, 1);
        if (!k || !v) continue;

        if (strcmp(k, "amount") == 0 && !found_amount) {
            char *endptr;
            long long amt = strtoll(v, &endptr, 10);
            if (endptr != v && *endptr == '\0' && amt >= 0) {
                out->amount = (int64_t)amt;
                found_amount = true;
            }
        } else if (strcmp(k, "closed_at") == 0 && out->closed_at == 0) {
            char *endptr;
            long long ts = strtoll(v, &endptr, 10);
            if (endptr != v && *endptr == '\0' && ts > 0)
                out->closed_at = (int64_t)ts;
        } else if (strcmp(k, "image") == 0 && !out->image) {
            out->image = v;
        } else if (strcmp(k, "summary") == 0 && !out->summary) {
            out->summary = v;
        } else if (strcmp(k, "r") == 0 && !out->url) {
            out->url = v;
        } else if (strcmp(k, "a") == 0 && !out->a_tag) {
            out->a_tag = v;
        }
    }

    return found_amount ? 0 : -ENOENT;
}

bool nostr_nip75_validate(const NostrEvent *ev) {
    if (!ev) return false;
    if (nostr_event_get_kind(ev) != NOSTR_NIP75_KIND_ZAP_GOAL)
        return false;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return false;

    bool has_amount = false;
    bool has_relays = false;
    size_t n = nostr_tags_size(tags);

    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;
        const char *k = nostr_tag_get(t, 0);
        if (!k) continue;

        if (strcmp(k, "amount") == 0) has_amount = true;
        if (strcmp(k, "relays") == 0) has_relays = true;
    }

    return has_amount && has_relays;
}

bool nostr_nip75_is_zap_goal(const NostrEvent *ev) {
    if (!ev) return false;
    return nostr_event_get_kind(ev) == NOSTR_NIP75_KIND_ZAP_GOAL;
}

int nostr_nip75_get_relays(const NostrEvent *ev, const char **relays,
                            size_t max, size_t *out_count) {
    if (!ev || !relays || !out_count) return -EINVAL;
    *out_count = 0;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return 0;

    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;
        const char *k = nostr_tag_get(t, 0);
        if (!k || strcmp(k, "relays") != 0) continue;

        /* "relays" tag: elements 1..N are relay URLs */
        size_t tsize = nostr_tag_size(t);
        size_t count = 0;
        for (size_t j = 1; j < tsize && count < max; ++j) {
            const char *v = nostr_tag_get(t, j);
            if (v && *v != '\0')
                relays[count++] = v;
        }
        *out_count = count;
        return 0;
    }

    return 0;
}

bool nostr_nip75_is_expired(const NostrNip75Goal *goal, int64_t now) {
    if (!goal || goal->closed_at == 0) return false;
    return now > goal->closed_at;
}

bool nostr_nip75_is_complete(int64_t current_msats, int64_t target_msats) {
    return current_msats >= target_msats && target_msats > 0;
}

double nostr_nip75_progress_percent(int64_t current_msats,
                                     int64_t target_msats) {
    if (target_msats <= 0) return 0.0;
    return ((double)current_msats / (double)target_msats) * 100.0;
}

int nostr_nip75_create_goal(NostrEvent *ev, const NostrNip75Goal *goal,
                             const char *content) {
    if (!ev || !goal || !content) return -EINVAL;
    if (goal->amount <= 0) return -EINVAL;

    nostr_event_set_kind(ev, NOSTR_NIP75_KIND_ZAP_GOAL);
    nostr_event_set_content(ev, content);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);

    /* Required: amount tag */
    char amt_buf[24];
    snprintf(amt_buf, sizeof(amt_buf), "%lld", (long long)goal->amount);
    nostr_tags_append(tags, nostr_tag_new("amount", amt_buf, NULL));

    /* Optional tags */
    if (goal->closed_at > 0) {
        char ts_buf[24];
        snprintf(ts_buf, sizeof(ts_buf), "%lld", (long long)goal->closed_at);
        nostr_tags_append(tags, nostr_tag_new("closed_at", ts_buf, NULL));
    }

    if (goal->image)
        nostr_tags_append(tags, nostr_tag_new("image", goal->image, NULL));

    if (goal->summary)
        nostr_tags_append(tags, nostr_tag_new("summary", goal->summary, NULL));

    if (goal->url)
        nostr_tags_append(tags, nostr_tag_new("r", goal->url, NULL));

    if (goal->a_tag)
        nostr_tags_append(tags, nostr_tag_new("a", goal->a_tag, NULL));

    return 0;
}

int nostr_nip75_add_relays(NostrEvent *ev, const char **relays,
                            size_t n_relays) {
    if (!ev || !relays || n_relays == 0) return -EINVAL;

    /*
     * Build a "relays" tag: ["relays", "wss://r1", "wss://r2", ...]
     * Start with nostr_tag_new for the first two elements, then append rest.
     */
    NostrTag *tag = nostr_tag_new("relays", relays[0], NULL);
    if (!tag) return -ENOMEM;

    for (size_t i = 1; i < n_relays; ++i) {
        if (relays[i])
            nostr_tag_append(tag, relays[i]);
    }

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, tag);
    return 0;
}
