#include "nostr/nip58/nip58.h"
#include <errno.h>
#include <string.h>

/* ============== Helpers ============== */

/*
 * Find the first tag with the given key and return its value (element 1).
 * Returns NULL if not found.
 */
static const char *find_tag_value(const NostrEvent *ev, const char *key) {
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return NULL;

    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;
        const char *k = nostr_tag_get(t, 0);
        if (k && strcmp(k, key) == 0)
            return nostr_tag_get(t, 1);
    }
    return NULL;
}

/*
 * Check if any tag with the given key exists.
 */
static bool has_tag(const NostrEvent *ev, const char *key) {
    return find_tag_value(ev, key) != NULL;
}

/* ============== Badge Definition (kind 30009) ============== */

int nostr_nip58_parse_definition(const NostrEvent *ev, NostrNip58BadgeDef *out) {
    if (!ev || !out) return -EINVAL;
    memset(out, 0, sizeof(*out));

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return -ENOENT;

    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;

        const char *k = nostr_tag_get(t, 0);
        const char *v = nostr_tag_get(t, 1);
        if (!k || !v) continue;

        if (strcmp(k, "d") == 0 && !out->identifier) {
            out->identifier = v;
        } else if (strcmp(k, "name") == 0 && !out->name) {
            out->name = v;
        } else if (strcmp(k, "description") == 0 && !out->description) {
            out->description = v;
        } else if (strcmp(k, "image") == 0 && !out->image_url) {
            out->image_url = v;
            if (nostr_tag_size(t) >= 3)
                out->image_dims = nostr_tag_get(t, 2);
        } else if (strcmp(k, "thumb") == 0 && !out->thumb_url) {
            out->thumb_url = v;
            if (nostr_tag_size(t) >= 3)
                out->thumb_dims = nostr_tag_get(t, 2);
        }
    }

    return out->identifier ? 0 : -ENOENT;
}

bool nostr_nip58_validate_definition(const NostrEvent *ev) {
    if (!ev) return false;
    if (nostr_event_get_kind(ev) != NOSTR_NIP58_KIND_BADGE_DEFINITION)
        return false;
    return has_tag(ev, "d");
}

int nostr_nip58_create_definition(NostrEvent *ev, const NostrNip58BadgeDef *def) {
    if (!ev || !def || !def->identifier) return -EINVAL;

    nostr_event_set_kind(ev, NOSTR_NIP58_KIND_BADGE_DEFINITION);
    nostr_event_set_content(ev, "");

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);

    /* Required: d tag */
    nostr_tags_append(tags, nostr_tag_new("d", def->identifier, NULL));

    /* Optional tags */
    if (def->name)
        nostr_tags_append(tags, nostr_tag_new("name", def->name, NULL));

    if (def->description)
        nostr_tags_append(tags, nostr_tag_new("description", def->description, NULL));

    if (def->image_url) {
        if (def->image_dims)
            nostr_tags_append(tags, nostr_tag_new("image", def->image_url,
                                                   def->image_dims, NULL));
        else
            nostr_tags_append(tags, nostr_tag_new("image", def->image_url, NULL));
    }

    if (def->thumb_url) {
        if (def->thumb_dims)
            nostr_tags_append(tags, nostr_tag_new("thumb", def->thumb_url,
                                                   def->thumb_dims, NULL));
        else
            nostr_tags_append(tags, nostr_tag_new("thumb", def->thumb_url, NULL));
    }

    return 0;
}

/* ============== Badge Award (kind 8) ============== */

int nostr_nip58_parse_award(const NostrEvent *ev, NostrNip58BadgeAward *out) {
    if (!ev || !out) return -EINVAL;
    memset(out, 0, sizeof(*out));

    const char *a = find_tag_value(ev, "a");
    if (!a) return -ENOENT;

    out->badge_ref = a;
    return 0;
}

bool nostr_nip58_validate_award(const NostrEvent *ev) {
    if (!ev) return false;
    if (nostr_event_get_kind(ev) != NOSTR_NIP58_KIND_BADGE_AWARD)
        return false;
    return has_tag(ev, "a") && has_tag(ev, "p");
}

size_t nostr_nip58_award_count_awardees(const NostrEvent *ev) {
    if (!ev) return 0;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return 0;

    size_t n = nostr_tags_size(tags);
    size_t count = 0;

    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;
        const char *k = nostr_tag_get(t, 0);
        if (k && strcmp(k, "p") == 0) ++count;
    }
    return count;
}

int nostr_nip58_award_get_awardees(const NostrEvent *ev,
                                    const char **pubkeys, size_t max,
                                    size_t *out_count) {
    if (!ev || !pubkeys || !out_count) return -EINVAL;
    *out_count = 0;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return 0;

    size_t n = nostr_tags_size(tags);
    size_t count = 0;

    for (size_t i = 0; i < n && count < max; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;
        const char *k = nostr_tag_get(t, 0);
        if (!k || strcmp(k, "p") != 0) continue;
        const char *v = nostr_tag_get(t, 1);
        if (v) pubkeys[count++] = v;
    }

    *out_count = count;
    return 0;
}

int nostr_nip58_create_award(NostrEvent *ev, const char *badge_ref,
                              const char **pubkeys, size_t n_pubkeys) {
    if (!ev || !badge_ref || !pubkeys || n_pubkeys == 0)
        return -EINVAL;

    nostr_event_set_kind(ev, NOSTR_NIP58_KIND_BADGE_AWARD);
    nostr_event_set_content(ev, "");

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);

    /* Badge definition reference */
    nostr_tags_append(tags, nostr_tag_new("a", badge_ref, NULL));

    /* Awardee pubkeys */
    for (size_t i = 0; i < n_pubkeys; ++i) {
        if (pubkeys[i])
            nostr_tags_append(tags, nostr_tag_new("p", pubkeys[i], NULL));
    }

    return 0;
}

/* ============== Profile Badges (kind 30008) ============== */

int nostr_nip58_parse_profile_badges(const NostrEvent *ev,
                                      NostrNip58ProfileBadge *entries,
                                      size_t max_entries, size_t *out_count) {
    if (!ev || !entries || !out_count) return -EINVAL;
    *out_count = 0;

    /* Verify d=profile_badges */
    const char *d = find_tag_value(ev, "d");
    if (!d || strcmp(d, "profile_badges") != 0)
        return -ENOENT;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return 0;

    size_t n = nostr_tags_size(tags);
    size_t count = 0;

    /* Parse alternating "a" then "e" tag pairs */
    const char *pending_badge_ref = NULL;

    for (size_t i = 0; i < n && count < max_entries; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;

        const char *k = nostr_tag_get(t, 0);
        const char *v = nostr_tag_get(t, 1);
        if (!k || !v) continue;

        if (strcmp(k, "a") == 0) {
            pending_badge_ref = v;
        } else if (strcmp(k, "e") == 0 && pending_badge_ref) {
            entries[count].badge_ref = pending_badge_ref;
            entries[count].award_id = v;
            entries[count].award_relay =
                (nostr_tag_size(t) >= 3) ? nostr_tag_get(t, 2) : NULL;
            ++count;
            pending_badge_ref = NULL;
        }
    }

    *out_count = count;
    return 0;
}

bool nostr_nip58_validate_profile_badges(const NostrEvent *ev) {
    if (!ev) return false;
    if (nostr_event_get_kind(ev) != NOSTR_NIP58_KIND_PROFILE_BADGES)
        return false;
    const char *d = find_tag_value(ev, "d");
    return d && strcmp(d, "profile_badges") == 0;
}

int nostr_nip58_create_profile_badges(NostrEvent *ev,
                                       const NostrNip58ProfileBadge *badges,
                                       size_t n_badges) {
    if (!ev) return -EINVAL;

    nostr_event_set_kind(ev, NOSTR_NIP58_KIND_PROFILE_BADGES);
    nostr_event_set_content(ev, "");

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);

    /* Required: d=profile_badges */
    nostr_tags_append(tags, nostr_tag_new("d", "profile_badges", NULL));

    /* Alternating a/e tag pairs */
    for (size_t i = 0; i < n_badges; ++i) {
        if (!badges[i].badge_ref || !badges[i].award_id)
            continue;

        nostr_tags_append(tags, nostr_tag_new("a", badges[i].badge_ref, NULL));

        if (badges[i].award_relay)
            nostr_tags_append(tags, nostr_tag_new("e", badges[i].award_id,
                                                   badges[i].award_relay, NULL));
        else
            nostr_tags_append(tags, nostr_tag_new("e", badges[i].award_id, NULL));
    }

    return 0;
}
