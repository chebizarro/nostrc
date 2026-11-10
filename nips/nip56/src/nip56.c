#include "nostr/nip56/nip56.h"
#include "nostr-kinds.h"
#include "codec-internal.h"

static const char *const categories[] = {"nudity", "malware", "profanity", "illegal", "spam", "impersonation", "other"};
const char *nostr_nip56_category_name(NostrNip56Category c) {
    return (unsigned)c < sizeof(categories) / sizeof(categories[0]) ? categories[c] : NULL;
}
int nostr_nip56_category_parse(const char *s, NostrNip56Category *out) {
    if (!s || !out) return -EINVAL;
    for (size_t i = 0; i < sizeof(categories) / sizeof(categories[0]); i++)
        if (!strcmp(s, categories[i])) { *out = (NostrNip56Category)i; return 0; }
    return -EINVAL;
}
static int target_type(const char *s) {
    if (!s) return -1;
    if (!strcmp(s, "p")) return NOSTR_NIP56_ACCOUNT;
    if (!strcmp(s, "e")) return NOSTR_NIP56_EVENT;
    if (!strcmp(s, "x")) return NOSTR_NIP56_BLOB;
    return -1;
}
int nostr_nip56_validate(const NostrEvent *ev, bool note) {
    if (!ev || nostr_event_get_kind(ev) != NOSTR_KIND_REPORTING) return -EINVAL;
    bool account = false, event = false, blobs[7] = {false}, events[7] = {false};
    NostrTags *tags = nostr_event_get_tags(ev);
    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
        NostrTag *t = nostr_tags_get(tags, i);
        int type = target_type(codec_field(t, 0));
        if (type < 0) continue;
        NostrNip56Category c;
        if (!codec_hex(codec_field(t, 1), 64) || nostr_nip56_category_parse(codec_field(t, 2), &c)) return -EINVAL;
        if (type == NOSTR_NIP56_ACCOUNT) account = true;
        else if (type == NOSTR_NIP56_EVENT) { event = true; events[c] = true; }
        else blobs[c] = true;
    }
    if (!account || (note && !event)) return -EINVAL;
    for (size_t i = 0; i < 7; i++) if (blobs[i] && !events[i]) return -EINVAL;
    return 0;
}
int nostr_nip56_parse(const NostrEvent *ev, bool note, NostrNip56Target *out, size_t max, size_t *count) {
    if (!count || (max && !out)) return -EINVAL;
    *count = 0;
    int rc = nostr_nip56_validate(ev, note);
    if (rc) return rc;
    NostrTags *tags = nostr_event_get_tags(ev);
    for (size_t i = 0; i < nostr_tags_size(tags); i++)
        if (target_type(codec_field(nostr_tags_get(tags, i), 0)) >= 0) (*count)++;
    if (*count > max) return -ENOSPC;
    size_t n = 0;
    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
        NostrTag *t = nostr_tags_get(tags, i);
        int type = target_type(codec_field(t, 0));
        if (type < 0) continue;
        out[n].type = (NostrNip56TargetType)type; out[n].id = codec_field(t, 1);
        nostr_nip56_category_parse(codec_field(t, 2), &out[n].category); n++;
    }
    return 0;
}
NostrEvent *nostr_nip56_build_unsigned(const char *author, int64_t at, const NostrNip56Target *targets, size_t count, bool note, const char *text) {
    if (!targets || !count || count > 4096) { errno = EINVAL; return NULL; }
    NostrTags *tags = nostr_tags_new(0);
    if (!tags) return NULL;
    const char *const names[] = {"p", "e", "x"};
    for (size_t i = 0; i < count; i++) {
        const char *category = nostr_nip56_category_name(targets[i].category);
        if ((unsigned)targets[i].type > NOSTR_NIP56_BLOB || !codec_hex(targets[i].id, 64) || !category) { errno = EINVAL; goto fail; }
        const char *fields[] = {names[targets[i].type], targets[i].id, category};
        int rc = codec_tag_append(tags, fields, 3);
        if (rc) { errno = -rc; goto fail; }
    }
    NostrEvent *ev = codec_event(NOSTR_KIND_REPORTING, author, at, tags, text ? text : "");
    if (ev && nostr_nip56_validate(ev, note)) { nostr_event_free(ev); errno = EINVAL; return NULL; }
    return ev;
fail:
    nostr_tags_free(tags); return NULL;
}
