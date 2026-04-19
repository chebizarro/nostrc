#include "nostr/nip28/nip28.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============== Helpers ============== */

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

/* ============== Channel Reference (kind 41) ============== */

int nostr_nip28_parse_channel_ref(const NostrEvent *ev,
                                   NostrNip28ChannelRef *out) {
    if (!ev || !out) return -EINVAL;
    memset(out, 0, sizeof(*out));

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return -ENOENT;

    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;
        const char *k = nostr_tag_get(t, 0);
        if (!k || strcmp(k, "e") != 0) continue;

        out->channel_id = nostr_tag_get(t, 1);
        out->relay = (nostr_tag_size(t) >= 3) ? nostr_tag_get(t, 2) : NULL;
        return 0;
    }
    return -ENOENT;
}

/* ============== Message Reference (kind 42) ============== */

int nostr_nip28_parse_message_ref(const NostrEvent *ev,
                                   NostrNip28MessageRef *out) {
    if (!ev || !out) return -EINVAL;
    memset(out, 0, sizeof(*out));

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return -ENOENT;

    size_t n = nostr_tags_size(tags);
    bool found_root = false;

    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;
        const char *k = nostr_tag_get(t, 0);
        if (!k || strcmp(k, "e") != 0) continue;

        const char *v = nostr_tag_get(t, 1);
        const char *relay = (nostr_tag_size(t) >= 3) ? nostr_tag_get(t, 2) : NULL;
        const char *marker = (nostr_tag_size(t) >= 4) ? nostr_tag_get(t, 3) : NULL;

        if (marker && strcmp(marker, "root") == 0) {
            out->channel_id = v;
            out->channel_relay = relay;
            found_root = true;
        } else if (marker && strcmp(marker, "reply") == 0) {
            out->reply_to = v;
            out->reply_relay = relay;
        } else if (!found_root) {
            /* First e tag without marker = root (fallback) */
            out->channel_id = v;
            out->channel_relay = relay;
            found_root = true;
        }
    }

    return found_root ? 0 : -ENOENT;
}

/* ============== Hide/Mute References ============== */

const char *nostr_nip28_get_hidden_message_id(const NostrEvent *ev) {
    if (!ev) return NULL;
    return find_tag_value(ev, "e");
}

const char *nostr_nip28_get_muted_pubkey(const NostrEvent *ev) {
    if (!ev) return NULL;
    return find_tag_value(ev, "p");
}

/* ============== Event Creation ============== */

int nostr_nip28_create_channel(NostrEvent *ev, const char *metadata_json) {
    if (!ev || !metadata_json) return -EINVAL;

    nostr_event_set_kind(ev, NOSTR_NIP28_KIND_CHANNEL_CREATE);
    nostr_event_set_content(ev, metadata_json);
    return 0;
}

int nostr_nip28_create_channel_metadata(NostrEvent *ev,
                                         const char *channel_id,
                                         const char *relay,
                                         const char *metadata_json) {
    if (!ev || !channel_id || !metadata_json) return -EINVAL;

    nostr_event_set_kind(ev, NOSTR_NIP28_KIND_CHANNEL_METADATA);
    nostr_event_set_content(ev, metadata_json);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (relay)
        nostr_tags_append(tags, nostr_tag_new("e", channel_id, relay, NULL));
    else
        nostr_tags_append(tags, nostr_tag_new("e", channel_id, NULL));

    return 0;
}

int nostr_nip28_create_message(NostrEvent *ev, const char *channel_id,
                                const char *relay, const char *content,
                                const char *reply_to) {
    if (!ev || !channel_id || !relay || !content) return -EINVAL;

    nostr_event_set_kind(ev, NOSTR_NIP28_KIND_CHANNEL_MESSAGE);
    nostr_event_set_content(ev, content);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);

    /* Root reference to channel */
    nostr_tags_append(tags, nostr_tag_new("e", channel_id, relay, "root", NULL));

    /* Optional reply reference */
    if (reply_to)
        nostr_tags_append(tags, nostr_tag_new("e", reply_to, relay, "reply", NULL));

    return 0;
}

int nostr_nip28_create_hide_message(NostrEvent *ev,
                                     const char *message_id,
                                     const char *reason) {
    if (!ev || !message_id) return -EINVAL;

    nostr_event_set_kind(ev, NOSTR_NIP28_KIND_HIDE_MESSAGE);

    /* Content is JSON with reason */
    if (reason) {
        /* Build simple JSON: {"reason":"..."} */
        size_t rlen = strlen(reason);
        /* Allocate enough for {"reason":"<reason>"}\0 */
        char *json = malloc(12 + rlen + 2 + 1);
        if (!json) return -ENOMEM;
        sprintf(json, "{\"reason\":\"%s\"}", reason);
        nostr_event_set_content(ev, json);
        free(json);
    } else {
        nostr_event_set_content(ev, "");
    }

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("e", message_id, NULL));

    return 0;
}

int nostr_nip28_create_mute_user(NostrEvent *ev, const char *pubkey,
                                  const char *reason) {
    if (!ev || !pubkey) return -EINVAL;

    nostr_event_set_kind(ev, NOSTR_NIP28_KIND_MUTE_USER);

    if (reason) {
        size_t rlen = strlen(reason);
        char *json = malloc(12 + rlen + 2 + 1);
        if (!json) return -ENOMEM;
        sprintf(json, "{\"reason\":\"%s\"}", reason);
        nostr_event_set_content(ev, json);
        free(json);
    } else {
        nostr_event_set_content(ev, "");
    }

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("p", pubkey, NULL));

    return 0;
}

/* ============== Validation ============== */

bool nostr_nip28_is_channel_event(const NostrEvent *ev) {
    if (!ev) return false;
    int kind = nostr_event_get_kind(ev);
    return kind >= NOSTR_NIP28_KIND_CHANNEL_CREATE &&
           kind <= NOSTR_NIP28_KIND_MUTE_USER;
}
