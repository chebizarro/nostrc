#ifndef NOSTR_NIPS_CODEC_INTERNAL_H
#define NOSTR_NIPS_CODEC_INTERNAL_H

/* Shared, private helpers for portable codecs. Jansson is the repository's
 * JSON backend; no signer, GLib, or transport dependencies live here. */
#include "nostr-event.h"
#include "nostr-tag.h"
#include <jansson.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline int codec_decimal(const char *s, int64_t max, int64_t *out) {
    if (!s || !*s || (s[0] == '0' && s[1])) return -EINVAL;
    int64_t n = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || n > (max - (*s - '0')) / 10) return -EINVAL;
        n = n * 10 + (*s - '0');
    }
    if (out) *out = n;
    return 0;
}

static inline bool codec_hex(const char *s, size_t n) {
    if (!s || strlen(s) != n) return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}

/* Split only the first two colons. The identifier may itself contain colons. */
static inline bool codec_address(const char *s, int *kind, const char **author,
                                 const char **identifier) {
    if (!s) return false;
    const char *a = strchr(s, ':');
    if (!a || a - s > 5 || a == s) return false;
    char k[6], pk[65];
    memcpy(k, s, (size_t)(a - s)); k[a - s] = 0;
    int64_t n;
    if (codec_decimal(k, 65535, &n) || strlen(a + 1) < 65 || a[65] != ':') return false;
    memcpy(pk, a + 1, 64); pk[64] = 0;
    if (!codec_hex(pk, 64)) return false;
    if (kind) *kind = (int)n;
    if (author) *author = a + 1;
    if (identifier) *identifier = a + 66;
    return true;
}

static inline bool codec_url(const char *s, bool relay) {
    if (!s) return false;
    const char *host = NULL;
    if (relay && !strncmp(s, "wss://", 6)) host = s + 6;
    else if (relay && !strncmp(s, "ws://", 5)) host = s + 5;
    else if (!relay && !strncmp(s, "https://", 8)) host = s + 8;
    else if (!relay && !strncmp(s, "http://", 7)) host = s + 7;
    if (!host || !*host || *host == '/' || *host == '?' || *host == '#') return false;
    for (const unsigned char *p = (const unsigned char *)host; *p; p++)
        if (*p <= 32 || *p == 127 || *p == '\\') return false;
    return true;
}

/* The core indexed getter terminates on an absent field. Codec validation
 * must instead distinguish absence from an explicitly empty string. */
static inline const char *codec_field(const NostrTag *tag, size_t index) {
    return tag && index < nostr_tag_size(tag) ? nostr_tag_get(tag, index) : NULL;
}

/* All array operations are checked: the core tag mutators return void and
 * otherwise allow allocation failures to silently shorten a replacement. */
static inline int codec_tag_append(NostrTags *tags, const char *const *fields, size_t n) {
    if (!tags || (n && !fields)) return -EINVAL;
    NostrTag *tag = nostr_tag_new(NULL);
    if (!tag) return -ENOMEM;
    for (size_t i = 0; i < n; i++) {
        if (!fields[i]) { nostr_tag_free(tag); return -EINVAL; }
        nostr_tag_append(tag, fields[i]);
        if (nostr_tag_size(tag) != i + 1 || !nostr_tag_get(tag, i) ||
            strcmp(nostr_tag_get(tag, i), fields[i])) {
            nostr_tag_free(tag); return -ENOMEM;
        }
    }
    size_t before = nostr_tags_size(tags);
    nostr_tags_append(tags, tag);
    if (nostr_tags_size(tags) != before + 1) { nostr_tag_free(tag); return -ENOMEM; }
    return 0;
}

static inline const char *codec_tag_value(const NostrEvent *ev, const char *key,
                                         size_t *count) {
    const char *v = NULL;
    size_t found = 0;
    NostrTags *tags = nostr_event_get_tags(ev);
    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
        NostrTag *t = nostr_tags_get(tags, i);
        const char *k = codec_field(t, 0);
        if (k && !strcmp(k, key)) { found++; v = codec_field(t, 1); }
    }
    if (count) *count = found;
    return v;
}

/* Takes tags even on failure. New event is unsigned and owns every field. */
static inline NostrEvent *codec_event(int kind, const char *author, int64_t at,
                                      NostrTags *tags, const char *content) {
    if (!codec_hex(author, 64) || at < 0 || !tags || !content) {
        nostr_tags_free(tags); errno = EINVAL; return NULL;
    }
    NostrEvent *ev = nostr_event_new();
    if (!ev) { nostr_tags_free(tags); errno = ENOMEM; return NULL; }
    nostr_event_set_kind(ev, kind);
    nostr_event_set_created_at(ev, at);
    nostr_event_set_tags(ev, tags);
    nostr_event_set_pubkey(ev, author);
    nostr_event_set_content(ev, content);
    if (!nostr_event_get_pubkey(ev) || !nostr_event_get_content(ev) ||
        strcmp(nostr_event_get_pubkey(ev), author) || strcmp(nostr_event_get_content(ev), content)) {
        nostr_event_free(ev); errno = ENOMEM; return NULL;
    }
    return ev;
}

typedef struct { char *data; size_t length; } CodecJsonBuffer;
static inline int codec_json_write(const char *bytes, size_t length, void *data) {
    CodecJsonBuffer *buffer = data;
    if (length > SIZE_MAX - buffer->length - 1) return -1;
    char *next = realloc(buffer->data, buffer->length + length + 1);
    if (!next) return -1;
    buffer->data = next;
    memcpy(next + buffer->length, bytes, length);
    buffer->length += length; next[buffer->length] = 0;
    return 0;
}
/* json_dumps may silently recover from a failed optional buffer shrink. Using
 * the backend's streaming serializer gives codecs an explicit error result for
 * every allocation they request, without implementing JSON escaping ourselves. */
static inline char *codec_json_dump(const json_t *value) {
    CodecJsonBuffer buffer = {0};
    if (!value || json_dump_callback(value, codec_json_write, &buffer, JSON_COMPACT | JSON_ENCODE_ANY)) {
        free(buffer.data); errno = ENOMEM; return NULL;
    }
    return buffer.data;
}

static inline json_t *codec_json_tags(const NostrTags *tags) {
    json_t *a = json_array();
    if (!a) return NULL;
    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
        NostrTag *t = nostr_tags_get(tags, i);
        json_t *row = json_array();
        if (!row) goto fail;
        for (size_t j = 0; j < nostr_tag_size(t); j++) {
            const char *s = nostr_tag_get(t, j);
            if (!s || json_array_append_new(row, json_string(s))) { json_decref(row); goto fail; }
        }
        if (json_array_append_new(a, row)) goto fail;
    }
    return a;
fail:
    json_decref(a); return NULL;
}

static inline NostrTags *codec_tags_json(const json_t *a) {
    if (!json_is_array(a)) { errno = EINVAL; return NULL; }
    NostrTags *tags = nostr_tags_new(0);
    if (!tags) { errno = ENOMEM; return NULL; }
    for (size_t i = 0; i < json_array_size(a); i++) {
        json_t *row = json_array_get(a, i);
        if (!json_is_array(row)) { errno = EINVAL; goto fail; }
        NostrTag *tag = nostr_tag_new(NULL);
        if (!tag) { errno = ENOMEM; goto fail; }
        for (size_t j = 0; j < json_array_size(row); j++) {
            json_t *v = json_array_get(row, j);
            const char *s = json_string_value(v);
            if (!s || strlen(s) != json_string_length(v)) {
                nostr_tag_free(tag); errno = EINVAL; goto fail;
            }
            nostr_tag_append(tag, s);
            if (nostr_tag_size(tag) != j + 1 || !nostr_tag_get(tag, j)) {
                nostr_tag_free(tag); errno = ENOMEM; goto fail;
            }
        }
        size_t before = nostr_tags_size(tags);
        nostr_tags_append(tags, tag);
        if (nostr_tags_size(tags) != before + 1) {
            nostr_tag_free(tag); errno = ENOMEM; goto fail;
        }
    }
    return tags;
fail:
    nostr_tags_free(tags); return NULL;
}
#endif
