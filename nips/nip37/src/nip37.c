#include "nostr/nip37/nip37.h"
#include "nostr-kinds.h"
#include "codec-internal.h"
#include <stdio.h>

int nostr_nip37_parse_envelope(const NostrEvent *ev, NostrNip37Envelope *out) {
    if (!ev || !out || nostr_event_get_kind(ev) != NOSTR_KIND_DRAFT_WRAP) return -EINVAL;
    NostrNip37Envelope value = {0};
    size_t n;
    value.identifier = codec_tag_value(ev, "d", &n);
    if (n != 1 || !value.identifier || strlen(value.identifier) > 256) return -EINVAL;
    const char *k = codec_tag_value(ev, "k", &n);
    int64_t kind;
    if (n != 1 || codec_decimal(k, 65535, &kind)) return -EINVAL;
    value.draft_kind = (int)kind;
    const char *expiration = codec_tag_value(ev, "expiration", &n);
    if (n > 1 || (n && codec_decimal(expiration, INT64_MAX, &value.expiration))) return -EINVAL;
    value.has_expiration = n == 1;
    const char *content = nostr_event_get_content(ev);
    if (!content) return -EINVAL;
    value.deleted = !*content;
    *out = value; return 0;
}
NostrEvent *nostr_nip37_build_envelope(const char *author, int64_t at, const char *id, int kind, const char *content, const int64_t *expiration) {
    if (!id || strlen(id) > 256 || kind < 0 || kind > 65535 || !content || (expiration && *expiration < 0)) { errno = EINVAL; return NULL; }
    NostrTags *tags = nostr_tags_new(0);
    if (!tags) return NULL;
    char k[16], expiry[32];
    snprintf(k, sizeof(k), "%d", kind);
    const char *dtag[] = {"d", id}, *ktag[] = {"k", k};
    int rc = codec_tag_append(tags, dtag, 2);
    if (!rc) rc = codec_tag_append(tags, ktag, 2);
    if (!rc && expiration) {
        snprintf(expiry, sizeof(expiry), "%lld", (long long)*expiration);
        const char *etag[] = {"expiration", expiry};
        rc = codec_tag_append(tags, etag, 2);
    }
    if (rc) { nostr_tags_free(tags); errno = -rc; return NULL; }
    return codec_event(NOSTR_KIND_DRAFT_WRAP, author, at, tags, content);
}
static bool valid_inner_tags(const NostrTags *tags) {
    if (nostr_tags_size(tags) > 4096) return false;
    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (nostr_tag_size(t) > 32) return false;
        for (size_t j = 0; j < nostr_tag_size(t); j++) {
            const char *s = nostr_tag_get(t, j);
            if (!s || strlen(s) > 16384) return false;
        }
    }
    return true;
}
NostrEvent *nostr_nip37_parse_draft(const char *plain, size_t len) {
    if (!plain || len > 60000 || memchr(plain, 0, len)) { errno = EINVAL; return NULL; }
    json_error_t error;
    json_t *root = json_loadb(plain, len, JSON_REJECT_DUPLICATES, &error);
    if (!json_is_object(root) || json_object_get(root, "sig")) goto invalid;
    json_t *author = json_object_get(root, "pubkey"), *at = json_object_get(root, "created_at"),
           *kind = json_object_get(root, "kind"), *content = json_object_get(root, "content");
    const char *pk = json_string_value(author), *text = json_string_value(content);
    if (!codec_hex(pk, 64) || !text || strlen(text) != json_string_length(content) ||
        !json_is_integer(at) || json_integer_value(at) < 0 || !json_is_integer(kind) ||
        json_integer_value(kind) < 0 || json_integer_value(kind) > 65535) goto invalid;
    NostrTags *tags = codec_tags_json(json_object_get(root, "tags"));
    if (!tags) { json_decref(root); return NULL; }
    if (!valid_inner_tags(tags)) { nostr_tags_free(tags); goto invalid; }
    NostrEvent *ev = codec_event((int)json_integer_value(kind), pk, json_integer_value(at), tags, text);
    json_decref(root); return ev;
invalid:
    json_decref(root); errno = EINVAL; return NULL;
}
char *nostr_nip37_serialize_draft(const NostrEvent *ev) {
    const char *pk = nostr_event_get_pubkey(ev), *text = nostr_event_get_content(ev),
               *sig = nostr_event_get_sig(ev);
    if (!ev || !codec_hex(pk, 64) || !text || (sig && *sig) ||
        nostr_event_get_created_at(ev) < 0 || nostr_event_get_kind(ev) < 0 || nostr_event_get_kind(ev) > 65535 ||
        !valid_inner_tags(nostr_event_get_tags(ev))) { errno = EINVAL; return NULL; }
    json_t *tags = codec_json_tags(nostr_event_get_tags(ev));
    if (!tags) return NULL;
    json_t *root = json_pack("{s:s,s:I,s:i,s:o,s:s}", "pubkey", pk,
        "created_at", (json_int_t)nostr_event_get_created_at(ev), "kind", nostr_event_get_kind(ev), "tags", tags, "content", text);
    if (!root) return NULL;
    char *plain = codec_json_dump(root); json_decref(root);
    if (plain && strlen(plain) > 60000) { free(plain); errno = E2BIG; return NULL; }
    return plain;
}
int nostr_nip37_parse_checkpoint(const NostrEvent *ev, NostrNip37Checkpoint *out) {
    if (!ev || !out || nostr_event_get_kind(ev) != NOSTR_KIND_DRAFT_CHECKPOINT) return -EINVAL;
    size_t n; int kind; const char *author, *id;
    const char *a = codec_tag_value(ev, "a", &n);
    if (n != 1 || !codec_address(a, &kind, &author, &id) || kind != NOSTR_KIND_DRAFT_WRAP || strlen(id) > 256) return -EINVAL;
    NostrNip37Checkpoint value = {0};
    memcpy(value.author, author, 64); value.identifier = id;
    *out = value; return 0;
}
NostrEvent *nostr_nip37_build_checkpoint(const char *author, int64_t at, const char *parent, const char *id, const char *content) {
    if (!codec_hex(parent, 64) || !id || strlen(id) > 256 || !content) { errno = EINVAL; return NULL; }
    char address[6 + 64 + 1 + 256 + 1];
    snprintf(address, sizeof(address), "%d:%s:%s", NOSTR_KIND_DRAFT_WRAP, parent, id);
    NostrTags *tags = nostr_tags_new(0);
    if (!tags) return NULL;
    const char *a[] = {"a", address};
    int rc = codec_tag_append(tags, a, 2);
    if (rc) { nostr_tags_free(tags); errno = -rc; return NULL; }
    return codec_event(NOSTR_KIND_DRAFT_CHECKPOINT, author, at, tags, content);
}
