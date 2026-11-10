#include "nostr/nip51/nip51.h"
#include "nostr-kinds.h"
#include "json.h"
#include "../../codec-internal.h"

struct NostrListTag { size_t count; char **fields; size_t bytes; };
struct NostrListDocument {
    int kind;
    NostrListTag **tags[2];
    size_t count[2], bytes[2];
    char *content;
    NostrListPrivateState state;
};

NostrListTag *nostr_nip51_tag_new(const char *const *fields, size_t n) {
    if ((n && !fields) || n > NOSTR_LIST_MAX_FIELDS) { errno = EINVAL; return NULL; }
    NostrListTag *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->fields = calloc(n ? n : 1, sizeof(char *));
    json_t *row = json_array();
    if (!t->fields || !row) goto fail;
    for (size_t i = 0; i < n; i++) {
        if (!fields[i] || strlen(fields[i]) > NOSTR_LIST_MAX_FIELD_BYTES) { errno = E2BIG; goto fail; }
        if (json_array_append_new(row, json_string(fields[i]))) { errno = EINVAL; goto fail; }
        t->fields[i] = strdup(fields[i]);
        if (!t->fields[i]) goto fail;
        t->count++;
    }
    char *s = codec_json_dump(row);
    if (!s) goto fail;
    t->bytes = strlen(s); free(s); json_decref(row);
    return t;
fail:
    json_decref(row); nostr_nip51_tag_free(t); return NULL;
}
void nostr_nip51_tag_free(NostrListTag *t) {
    if (!t) return;
    for (size_t i = 0; i < t->count; i++) free(t->fields[i]);
    free(t->fields); free(t);
}
size_t nostr_nip51_tag_size(const NostrListTag *t) { return t ? t->count : 0; }
const char *nostr_nip51_tag_field(const NostrListTag *t, size_t i) { return t && i < t->count ? t->fields[i] : NULL; }
static size_t max_tags(const NostrListDocument *d) {
    return d->kind == NOSTR_KIND_FOLLOW_LIST ? NOSTR_LIST_MAX_FOLLOW_TAGS : NOSTR_LIST_MAX_TAGS;
}
static size_t max_bytes(const NostrListDocument *d) {
    return d->kind == NOSTR_KIND_FOLLOW_LIST ? NOSTR_LIST_MAX_FOLLOW_BYTES : NOSTR_LIST_MAX_EVENT_BYTES;
}
static int edit_allowed(const NostrListDocument *d, bool priv, size_t removed, const NostrListTag *t) {
    if (!d || (priv && d->state != NOSTR_LIST_PRIVATE_EMPTY && d->state != NOSTR_LIST_PRIVATE_DECODED)) return -EACCES;
    const char *name = nostr_nip51_tag_field(t, 0), *value = nostr_nip51_tag_field(t, 1);
    if (name && !strcmp(name, "d") && (!value || strlen(value) > NOSTR_LIST_MAX_IDENTIFIER_BYTES)) return -EINVAL;
    size_t bytes = d->bytes[priv] - removed + (t ? t->bytes + 1 : 0);
    size_t private_bytes = bytes ? bytes + 1 : 2;
    size_t content_bytes = !priv && d->state != NOSTR_LIST_PRIVATE_DECODED ? strlen(d->content) : 0;
    if ((priv && private_bytes > NOSTR_LIST_MAX_PRIVATE_BYTES) ||
        bytes + d->bytes[!priv] + content_bytes > max_bytes(d)) return -E2BIG;
    return 0;
}
int nostr_nip51_document_append(NostrListDocument *d, bool priv, const NostrListTag *t) {
    if (!d || !t) return -EINVAL;
    if (d->count[0] + d->count[1] >= max_tags(d)) return -E2BIG;
    int rc = edit_allowed(d, priv, 0, t);
    if (rc) return rc;
    NostrListTag *copy = nostr_nip51_tag_new((const char *const *)t->fields, t->count);
    if (!copy) return -ENOMEM;
    NostrListTag **next = realloc(d->tags[priv], (d->count[priv] + 1) * sizeof(*next));
    if (!next) { nostr_nip51_tag_free(copy); return -ENOMEM; }
    d->tags[priv] = next; next[d->count[priv]++] = copy;
    d->bytes[priv] += copy->bytes + 1;
    if (priv) d->state = NOSTR_LIST_PRIVATE_DECODED;
    return 0;
}
int nostr_nip51_document_replace(NostrListDocument *d, bool priv, size_t i, const NostrListTag *t) {
    if (!d || !t || i >= d->count[priv]) return -EINVAL;
    int rc = edit_allowed(d, priv, d->tags[priv][i]->bytes + 1, t);
    if (rc) return rc;
    NostrListTag *copy = nostr_nip51_tag_new((const char *const *)t->fields, t->count);
    if (!copy) return -ENOMEM;
    d->bytes[priv] -= d->tags[priv][i]->bytes + 1;
    nostr_nip51_tag_free(d->tags[priv][i]); d->tags[priv][i] = copy;
    d->bytes[priv] += copy->bytes + 1;
    if (priv) d->state = NOSTR_LIST_PRIVATE_DECODED;
    return 0;
}
int nostr_nip51_document_remove(NostrListDocument *d, bool priv, size_t i) {
    if (!d || i >= d->count[priv]) return -EINVAL;
    int rc = edit_allowed(d, priv, 0, NULL);
    if (rc) return rc;
    d->bytes[priv] -= d->tags[priv][i]->bytes + 1;
    nostr_nip51_tag_free(d->tags[priv][i]);
    memmove(d->tags[priv] + i, d->tags[priv] + i + 1, (d->count[priv] - i - 1) * sizeof(*d->tags[priv]));
    d->count[priv]--;
    if (priv) d->state = NOSTR_LIST_PRIVATE_DECODED;
    return 0;
}
void nostr_nip51_document_free(NostrListDocument *d) {
    if (!d) return;
    for (int p = 0; p < 2; p++) {
        for (size_t i = 0; i < d->count[p]; i++) nostr_nip51_tag_free(d->tags[p][i]);
        free(d->tags[p]);
    }
    free(d->content); free(d);
}
NostrListDocument *nostr_nip51_document_new(int kind, const char *identifier) {
    const NostrListSchema *s = nostr_nip51_schema_lookup(kind);
    if (!s || (nostr_nip51_schema_requires_identifier(s) && !identifier) ||
        (!nostr_nip51_schema_requires_identifier(s) && identifier) ||
        (identifier && strlen(identifier) > NOSTR_LIST_MAX_IDENTIFIER_BYTES)) { errno = EINVAL; return NULL; }
    NostrListDocument *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->kind = kind; d->content = strdup("");
    if (!d->content) goto fail;
    if (identifier) {
        const char *f[] = {"d", identifier};
        NostrListTag *t = nostr_nip51_tag_new(f, 2);
        int rc = t ? nostr_nip51_document_append(d, false, t) : -ENOMEM;
        nostr_nip51_tag_free(t);
        if (rc) { errno = -rc; goto fail; }
    }
    return d;
fail:
    nostr_nip51_document_free(d); return NULL;
}
int nostr_nip51_document_kind(const NostrListDocument *d) { return d ? d->kind : -1; }
const char *nostr_nip51_document_identifier(const NostrListDocument *d) {
    if (!d) return NULL;
    for (size_t i = 0; i < d->count[0]; i++) {
        const NostrListTag *t = d->tags[0][i];
        if (t->count && !strcmp(t->fields[0], "d")) return nostr_nip51_tag_field(t, 1);
    }
    return "";
}
bool nostr_nip51_document_deprecated(const NostrListDocument *d) {
    const char *id = nostr_nip51_document_identifier(d);
    return d && id && ((d->kind == NOSTR_KIND_CATEGORIZED_PEOPLE_LIST && !strcmp(id, "mute")) ||
        (d->kind == NOSTR_KIND_CATEGORIZED_BOOKMARKS_LIST &&
         (!strcmp(id, "pin") || !strcmp(id, "bookmark") || !strcmp(id, "communities"))) ||
        (d->kind == NOSTR_KIND_BADGE_SETS && !strcmp(id, "profile_badges")));
}
size_t nostr_nip51_document_tag_count(const NostrListDocument *d, bool p) { return d ? d->count[p] : 0; }
const NostrListTag *nostr_nip51_document_tag(const NostrListDocument *d, bool p, size_t i) { return d && i < d->count[p] ? d->tags[p][i] : NULL; }
NostrListPrivateState nostr_nip51_document_private_state(const NostrListDocument *d) { return d ? d->state : NOSTR_LIST_PRIVATE_UNREADABLE; }
void nostr_nip51_document_mark_unreadable(NostrListDocument *d) { if (d) d->state = NOSTR_LIST_PRIVATE_UNREADABLE; }

NostrListDocument *nostr_nip51_document_parse_event(const NostrEvent *ev) {
    if (!ev) { errno = EINVAL; return NULL; }
    int kind = nostr_event_get_kind(ev);
    const NostrListSchema *s = nostr_nip51_schema_lookup(kind);
    if (!s) { errno = ENOTSUP; return NULL; }
    /* Parse does not authenticate a revision. The service verifies signatures. */
    const char *content = nostr_event_get_content(ev);
    if (!content) content = "";
    size_t limit = kind == NOSTR_KIND_FOLLOW_LIST ? NOSTR_LIST_MAX_FOLLOW_BYTES : NOSTR_LIST_MAX_EVENT_BYTES;
    if (strlen(content) > limit) { errno = E2BIG; return NULL; }
    char *wire = nostr_event_serialize(ev);
    if (!wire) { errno = ENOMEM; return NULL; }
    size_t length = strlen(wire); free(wire);
    if (length > limit) { errno = E2BIG; return NULL; }
    NostrListDocument *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->kind = kind; d->content = strdup(content);
    if (!d->content) goto fail;
    NostrTags *tags = nostr_event_get_tags(ev);
    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
        NostrTag *raw = nostr_tags_get(tags, i);
        size_t n = nostr_tag_size(raw);
        if (n > NOSTR_LIST_MAX_FIELDS) { errno = E2BIG; goto fail; }
        const char *fields[NOSTR_LIST_MAX_FIELDS];
        for (size_t j = 0; j < n; j++) fields[j] = nostr_tag_get(raw, j);
        NostrListTag *t = nostr_nip51_tag_new(fields, n);
        int rc = t ? nostr_nip51_document_append(d, false, t) : -(errno ? errno : ENOMEM);
        nostr_nip51_tag_free(t);
        if (rc) { errno = -rc; goto fail; }
    }
    d->state = !*content || kind == NOSTR_KIND_FOLLOW_LIST || kind == NOSTR_KIND_RELAY_LIST_METADATA ||
        kind == NOSTR_KIND_USER_SERVER_LIST || kind == NOSTR_KIND_RELEASE_ARTIFACT_SETS ? NOSTR_LIST_PRIVATE_EMPTY :
        strstr(content, "iv") ? NOSTR_LIST_PRIVATE_LEGACY_NIP04 : NOSTR_LIST_PRIVATE_ENCRYPTED_NIP44;
    return d;
fail:
    nostr_nip51_document_free(d); return NULL;
}

int nostr_nip51_document_parse_private_tags(NostrListDocument *d, const char *plain, size_t len) {
    if (!d || !plain || len > NOSTR_LIST_MAX_PRIVATE_BYTES || memchr(plain, 0, len)) return -EINVAL;
    json_error_t error;
    json_t *root = json_loadb(plain, len, JSON_REJECT_DUPLICATES, &error);
    if (!json_is_array(root)) { json_decref(root); return -EINVAL; }
    NostrListDocument next = { .kind = d->kind, .content = "" };
    int rc = 0;
    for (size_t i = 0; i < json_array_size(root); i++) {
        json_t *row = json_array_get(root, i);
        if (!json_is_array(row) || json_array_size(row) > NOSTR_LIST_MAX_FIELDS) { rc = -EINVAL; break; }
        const char *fields[NOSTR_LIST_MAX_FIELDS];
        size_t n = json_array_size(row);
        for (size_t j = 0; j < n; j++) {
            json_t *v = json_array_get(row, j);
            fields[j] = json_string_value(v);
            if (!fields[j] || strlen(fields[j]) != json_string_length(v)) { rc = -EINVAL; break; }
        }
        if (rc) break;
        NostrListTag *t = nostr_nip51_tag_new(fields, n);
        rc = t ? nostr_nip51_document_append(&next, true, t) : -(errno ? errno : ENOMEM);
        nostr_nip51_tag_free(t);
        if (rc) break;
    }
    json_decref(root);
    if (!rc && next.count[1] + d->count[0] > max_tags(d)) rc = -E2BIG;
    if (rc) {
        for (size_t i = 0; i < next.count[1]; i++) nostr_nip51_tag_free(next.tags[1][i]);
        free(next.tags[1]); return rc;
    }
    for (size_t i = 0; i < d->count[1]; i++) nostr_nip51_tag_free(d->tags[1][i]);
    free(d->tags[1]); d->tags[1] = next.tags[1]; d->count[1] = next.count[1]; d->bytes[1] = next.bytes[1];
    d->state = NOSTR_LIST_PRIVATE_DECODED;
    return 0;
}
char *nostr_nip51_document_serialize_private_tags(const NostrListDocument *d) {
    if (!d || (d->state != NOSTR_LIST_PRIVATE_EMPTY && d->state != NOSTR_LIST_PRIVATE_DECODED)) { errno = EACCES; return NULL; }
    json_t *a = json_array();
    if (!a) return NULL;
    for (size_t i = 0; i < d->count[1]; i++) {
        NostrListTag *t = d->tags[1][i];
        json_t *row = json_array();
        if (!row) goto fail;
        for (size_t j = 0; j < t->count; j++)
            if (json_array_append_new(row, json_string(t->fields[j]))) { json_decref(row); goto fail; }
        if (json_array_append_new(a, row)) goto fail;
    }
    char *s = codec_json_dump(a); json_decref(a);
    if (s && strlen(s) > NOSTR_LIST_MAX_PRIVATE_BYTES) { free(s); errno = E2BIG; return NULL; }
    return s;
fail:
    json_decref(a); errno = ENOMEM; return NULL;
}
int nostr_nip51_document_validate(const NostrListDocument *d) {
    if (!d) return -EINVAL;
    const NostrListSchema *s = nostr_nip51_schema_lookup(d->kind);
    if (!s) return -ENOTSUP;
    if (nostr_nip51_schema_privacy(s) == NOSTR_LIST_PRIVACY_PRIVATE && d->count[0]) return -EINVAL;
    size_t identifiers = 0;
    for (int p = 0; p < 2; p++) for (size_t i = 0; i < d->count[p]; i++) {
        const NostrListTag *t = d->tags[p][i];
        const char *key = nostr_nip51_tag_field(t, 0);
        if (key && !strcmp(key, "d")) {
            if (p || t->count < 2 || strlen(t->fields[1]) > NOSTR_LIST_MAX_IDENTIFIER_BYTES) return -EINVAL;
            identifiers++;
            if (d->kind == NOSTR_KIND_MUTE_SETS && codec_decimal(t->fields[1], 65535, NULL)) return -EINVAL;
        }
        if (nostr_nip51_validate_item(s, t) == NOSTR_LIST_ITEM_INVALID) return -EINVAL;
        if (nostr_nip51_schema_privacy(s) == NOSTR_LIST_PRIVACY_PRIVATE && !p && key &&
            nostr_nip51_validate_item(s, t) == NOSTR_LIST_ITEM_VALID && strcmp(key, "d")) return -EINVAL;
        if (nostr_nip51_schema_privacy(s) == NOSTR_LIST_PRIVACY_PUBLIC && p) return -EINVAL;
    }
    if (identifiers != (size_t)nostr_nip51_schema_requires_identifier(s)) return -EINVAL;
    if (nostr_nip51_schema_privacy(s) == NOSTR_LIST_PRIVACY_PUBLIC &&
        d->kind != NOSTR_KIND_FOLLOW_LIST && d->kind != NOSTR_KIND_RELAY_LIST_METADATA && *d->content) return -EINVAL;
    return 0;
}
NostrEvent *nostr_nip51_document_build_unsigned(const NostrListDocument *d, const char *author, int64_t at, const char *encrypted) {
    int rc = nostr_nip51_document_validate(d);
    if (rc || nostr_nip51_schema_read_only(nostr_nip51_schema_lookup(d ? d->kind : -1)) ||
        (d && d->state == NOSTR_LIST_PRIVATE_UNREADABLE)) { errno = rc ? -rc : EACCES; return NULL; }
    if ((d->state == NOSTR_LIST_PRIVATE_ENCRYPTED_NIP44 || d->state == NOSTR_LIST_PRIVATE_LEGACY_NIP04) &&
        encrypted && strcmp(encrypted, d->content)) { errno = EACCES; return NULL; }
    const char *content = encrypted ? encrypted : d->content;
    if ((d->state == NOSTR_LIST_PRIVATE_DECODED && (!encrypted || (d->count[1] && !*encrypted))) ||
        (nostr_nip51_schema_privacy(nostr_nip51_schema_lookup(d->kind)) == NOSTR_LIST_PRIVACY_PUBLIC && *content)) { errno = EINVAL; return NULL; }
    if (strlen(content) > max_bytes(d)) { errno = E2BIG; return NULL; }
    NostrTags *tags = nostr_tags_new(0);
    if (!tags) return NULL;
    for (size_t i = 0; i < d->count[0]; i++) {
        NostrListTag *t = d->tags[0][i];
        rc = codec_tag_append(tags, (const char *const *)t->fields, t->count);
        if (rc) { nostr_tags_free(tags); errno = -rc; return NULL; }
    }
    NostrEvent *ev = codec_event(d->kind, author, at, tags, content);
    if (!ev) return NULL;
    /* Measure the eventual signed shape, including id/sig keys and commas,
     * without signing or mutating the unsigned result. */
    char id[65], sig[129]; memset(id, '0', 64); id[64] = 0; memset(sig, '0', 128); sig[128] = 0;
    NostrEvent signed_shape = *ev; signed_shape.id = id; signed_shape.sig = sig;
    char *wire = nostr_event_serialize(&signed_shape);
    if (!wire || strlen(wire) > max_bytes(d)) {
        errno = wire ? E2BIG : ENOMEM; free(wire); nostr_event_free(ev); return NULL;
    }
    free(wire); return ev;
}
