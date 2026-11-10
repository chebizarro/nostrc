#include "nostr/nip51/nip51.h"
#include "nostr/nip44/nip44.h"
#include "nostr-kinds.h"
#include "nostr-keys.h"
#include "nostr-utils.h"
#include "../../codec-internal.h"
#include <time.h>

NostrList *nostr_nip51_list_new(void) {
    NostrList *list = calloc(1, sizeof(*list));
    if (!list) return NULL;
    list->capacity = 8;
    list->entries = calloc(list->capacity, sizeof(*list->entries));
    if (!list->entries) { free(list); return NULL; }
    return list;
}
void nostr_nip51_list_free(NostrList *list) {
    if (!list) return;
    nostr_nip51_free_entries(list->entries, list->count);
    free(list->identifier); free(list->title); free(list->description); free(list);
}
NostrListEntry *nostr_nip51_entry_new(const char *name, const char *value, const char *extra, bool priv) {
    if (!name || !value) { errno = EINVAL; return NULL; }
    NostrListEntry *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->tag_name = strdup(name); e->value = strdup(value);
    e->extra = extra ? strdup(extra) : NULL; e->is_private = priv;
    if (!e->tag_name || !e->value || (extra && !e->extra)) { nostr_nip51_entry_free(e); return NULL; }
    return e;
}
void nostr_nip51_entry_free(NostrListEntry *e) {
    if (!e) return;
    free(e->tag_name); free(e->value); free(e->extra); free(e);
}
/* The legacy void mutators cannot report failure directly. Invalidate their
 * builder on failure so a later raw-key call cannot sign a partial replacement.
 * New callers use the transactional, error-returning document API instead. */
static void invalidate_list(NostrList *list) {
    if (!list) return;
    nostr_nip51_free_entries(list->entries, list->count);
    list->entries = NULL; list->count = 0; list->capacity = 0;
}
static int list_add(NostrList *list, NostrListEntry *e) {
    if (!list || !list->entries || !e) return -EINVAL;
    if (list->count >= NOSTR_LIST_MAX_TAGS) return -E2BIG;
    if (list->count >= list->capacity) {
        size_t cap = list->capacity ? list->capacity * 2 : 8;
        NostrListEntry **next = realloc(list->entries, cap * sizeof(*next));
        if (!next) return -ENOMEM;
        list->entries = next; list->capacity = cap;
    }
    list->entries[list->count++] = e; return 0;
}
void nostr_nip51_list_add_entry(NostrList *list, NostrListEntry *e) {
    int rc = list_add(list, e);
    if (rc) { nostr_nip51_entry_free(e); invalidate_list(list); errno = -rc; }
}
static void set_string(NostrList *list, char **dest, const char *s) {
    char *next = s ? strdup(s) : NULL;
    if (s && !next) { invalidate_list(list); errno = ENOMEM; return; }
    free(*dest); *dest = next;
}
void nostr_nip51_list_set_identifier(NostrList *list, const char *s) { if (list) set_string(list, &list->identifier, s); }
void nostr_nip51_list_set_title(NostrList *list, const char *s) { if (list) set_string(list, &list->title, s); }
void nostr_nip51_mute_user(NostrList *list, const char *s, bool p) { nostr_nip51_list_add_entry(list, nostr_nip51_entry_new("p", s, NULL, p)); }
void nostr_nip51_mute_word(NostrList *list, const char *s, bool p) { nostr_nip51_list_add_entry(list, nostr_nip51_entry_new("word", s, NULL, p)); }
void nostr_nip51_mute_hashtag(NostrList *list, const char *s, bool p) { nostr_nip51_list_add_entry(list, nostr_nip51_entry_new("t", s, NULL, p)); }
void nostr_nip51_mute_event(NostrList *list, const char *s, bool p) { nostr_nip51_list_add_entry(list, nostr_nip51_entry_new("e", s, NULL, p)); }
void nostr_nip51_bookmark_event(NostrList *list, const char *s, const char *r, bool p) { nostr_nip51_list_add_entry(list, nostr_nip51_entry_new("e", s, r, p)); }
void nostr_nip51_bookmark_url(NostrList *list, const char *s, bool p) { nostr_nip51_list_add_entry(list, nostr_nip51_entry_new("r", s, NULL, p)); }

static int append_entry(NostrListDocument *doc, const NostrListEntry *e) {
    if (!e) return -EINVAL;
    const char *fields[] = {e->tag_name, e->value, e->extra};
    NostrListTag *tag = nostr_nip51_tag_new(fields, e->extra ? 3 : 2);
    if (!tag) return -(errno ? errno : ENOMEM);
    int rc = nostr_nip51_document_append(doc, e->is_private, tag);
    nostr_nip51_tag_free(tag); return rc;
}
char *nostr_nip51_encrypt_private_entries(NostrListEntry **entries, size_t count, const char *sk_hex) {
    if ((!entries && count) || !count || !sk_hex) { errno = EINVAL; return NULL; }
    NostrListDocument *doc = nostr_nip51_document_new(NOSTR_KIND_MUTE_LIST, NULL);
    if (!doc) return NULL;
    for (size_t i = 0; i < count; i++) {
        if (!entries[i]) { nostr_nip51_document_free(doc); errno = EINVAL; return NULL; }
        NostrListEntry e = *entries[i]; e.is_private = true;
        int rc = append_entry(doc, &e);
        if (rc) { nostr_nip51_document_free(doc); errno = -rc; return NULL; }
    }
    char *plain = nostr_nip51_document_serialize_private_tags(doc);
    nostr_nip51_document_free(doc);
    if (!plain) return NULL;
    char *pk_hex = nostr_key_get_public(sk_hex), *encrypted = NULL;
    unsigned char sk[32] = {0}, pk[32];
    int rc = -EINVAL;
    if (pk_hex && codec_hex(sk_hex, 64) && nostr_hex2bin(sk, sk_hex, 32) && nostr_hex2bin(pk, pk_hex, 32))
        rc = nostr_nip44_encrypt_v2(sk, pk, (const uint8_t *)plain, strlen(plain), &encrypted);
    memset(sk, 0, sizeof(sk)); free(pk_hex); free(plain);
    if (rc || !encrypted || !*encrypted) { free(encrypted); errno = EIO; return NULL; }
    return encrypted;
}
static NostrListEntry **document_entries(const NostrListDocument *doc, bool priv, size_t *count) {
    size_t n = nostr_nip51_document_tag_count(doc, priv);
    NostrListEntry **entries = calloc(n ? n : 1, sizeof(*entries));
    if (!entries) return NULL;
    for (size_t i = 0; i < n; i++) {
        const NostrListTag *t = nostr_nip51_document_tag(doc, priv, i);
        size_t arity = nostr_nip51_tag_size(t);
        if (arity < 2 || arity > 3) { errno = ENOTSUP; goto fail; }
        entries[i] = nostr_nip51_entry_new(nostr_nip51_tag_field(t, 0), nostr_nip51_tag_field(t, 1), nostr_nip51_tag_field(t, 2), priv);
        if (!entries[i]) goto fail;
    }
    *count = n; return entries;
fail:
    nostr_nip51_free_entries(entries, n); return NULL;
}
NostrListEntry **nostr_nip51_decrypt_private_entries(const char *content, const char *sk_hex, size_t *count) {
    if (!count) { errno = EINVAL; return NULL; }
    *count = 0;
    if (!content || !*content || !codec_hex(sk_hex, 64)) { errno = EINVAL; return NULL; }
    char *pk_hex = nostr_key_get_public(sk_hex);
    unsigned char sk[32] = {0}, pk[32];
    uint8_t *plain = NULL; size_t len = 0;
    int rc = -EINVAL;
    if (pk_hex && nostr_hex2bin(sk, sk_hex, 32) && nostr_hex2bin(pk, pk_hex, 32))
        rc = nostr_nip44_decrypt_v2(sk, pk, content, &plain, &len);
    memset(sk, 0, sizeof(sk)); free(pk_hex);
    if (rc || !plain) { free(plain); errno = EIO; return NULL; }
    NostrListDocument *doc = nostr_nip51_document_new(NOSTR_KIND_MUTE_LIST, NULL);
    if (!doc) { free(plain); return NULL; }
    rc = nostr_nip51_document_parse_private_tags(doc, (const char *)plain, len);
    free(plain);
    NostrListEntry **entries = rc ? NULL : document_entries(doc, true, count);
    nostr_nip51_document_free(doc);
    if (rc) errno = -rc;
    return entries;
}
void nostr_nip51_free_entries(NostrListEntry **entries, size_t n) {
    if (!entries) return;
    for (size_t i = 0; i < n; i++) nostr_nip51_entry_free(entries[i]);
    free(entries);
}
static int append_metadata(NostrListDocument *d, const char *key, const char *value) {
    if (!value) return 0;
    const char *f[] = {key, value};
    NostrListTag *t = nostr_nip51_tag_new(f, 2);
    if (!t) return -ENOMEM;
    int rc = nostr_nip51_document_append(d, false, t);
    nostr_nip51_tag_free(t); return rc;
}
NostrEvent *nostr_nip51_create_list(int kind, NostrList *list, const char *sk_hex) {
    if (!list || !codec_hex(sk_hex, 64) || list->count > NOSTR_LIST_MAX_TAGS || !list->entries) { errno = EINVAL; return NULL; }
    NostrListDocument *d = nostr_nip51_document_new(kind, list->identifier);
    if (!d) return NULL;
    int rc = append_metadata(d, "title", list->title);
    if (!rc) rc = append_metadata(d, "description", list->description);
    size_t npriv = 0;
    for (size_t i = 0; !rc && i < list->count; i++) {
        rc = append_entry(d, list->entries[i]);
        if (!rc && list->entries[i]->is_private) npriv++;
    }
    if (!rc) rc = nostr_nip51_document_validate(d);
    char *pk = NULL, *encrypted = NULL;
    if (!rc) { pk = nostr_key_get_public(sk_hex); if (!pk) rc = -EINVAL; }
    if (!rc && npriv) {
        NostrListEntry **entries = calloc(npriv, sizeof(*entries));
        if (!entries) rc = -ENOMEM;
        else {
            size_t j = 0;
            for (size_t i = 0; i < list->count; i++) if (list->entries[i]->is_private) entries[j++] = list->entries[i];
            encrypted = nostr_nip51_encrypt_private_entries(entries, npriv, sk_hex);
            free(entries);
            if (!encrypted) rc = -EIO;
        }
    }
    NostrEvent *ev = rc ? NULL : nostr_nip51_document_build_unsigned(d, pk, (int64_t)time(NULL), encrypted);
    free(pk); free(encrypted); nostr_nip51_document_free(d);
    if (rc) errno = -rc;
    if (ev && nostr_event_sign(ev, sk_hex)) { nostr_event_free(ev); return NULL; }
    return ev;
}
NostrEvent *nostr_nip51_create_mute_list(NostrList *l, const char *sk) { return nostr_nip51_create_list(NOSTR_KIND_MUTE_LIST, l, sk); }
NostrEvent *nostr_nip51_create_bookmark_list(NostrList *l, const char *sk) { return nostr_nip51_create_list(NOSTR_KIND_BOOKMARK_LIST, l, sk); }
NostrEvent *nostr_nip51_create_pin_list(NostrList *l, const char *sk) { return nostr_nip51_create_list(NOSTR_KIND_PIN_LIST, l, sk); }
NostrList *nostr_nip51_parse_list(NostrEvent *ev, const char *sk) {
    NostrListDocument *d = nostr_nip51_document_parse_event(ev);
    if (!d) return NULL;
    NostrList *l = nostr_nip51_list_new();
    if (!l) goto fail;
    for (size_t i = 0; i < nostr_nip51_document_tag_count(d, false); i++) {
        const NostrListTag *t = nostr_nip51_document_tag(d, false, i);
        const char *name = nostr_nip51_tag_field(t, 0), *value = nostr_nip51_tag_field(t, 1);
        if (!name || !value || nostr_nip51_tag_size(t) > 3) { errno = ENOTSUP; goto fail; }
        char **dest = !strcmp(name, "d") ? &l->identifier : !strcmp(name, "title") ? &l->title : !strcmp(name, "description") ? &l->description : NULL;
        if (dest) {
            if (nostr_nip51_tag_size(t) != 2 || *dest) { errno = ENOTSUP; goto fail; }
            *dest = strdup(value); if (!*dest) goto fail;
        } else {
            NostrListEntry *e = nostr_nip51_entry_new(name, value, nostr_nip51_tag_field(t, 2), false);
            if (!e) goto fail;
            int rc = list_add(l, e);
            if (rc) { nostr_nip51_entry_free(e); errno = -rc; goto fail; }
        }
    }
    const char *content = nostr_event_get_content(ev);
    if (sk && content && *content) {
        size_t n = 0;
        NostrListEntry **entries = nostr_nip51_decrypt_private_entries(content, sk, &n);
        if (!entries) goto fail;
        for (size_t i = 0; i < n; i++) {
            int rc = list_add(l, entries[i]);
            if (rc) {
                for (size_t j = i; j < n; j++) nostr_nip51_entry_free(entries[j]);
                free(entries); errno = -rc; goto fail; }
        }
        free(entries);
    }
    nostr_nip51_document_free(d); return l;
fail:
    nostr_nip51_document_free(d); nostr_nip51_list_free(l); return NULL;
}
