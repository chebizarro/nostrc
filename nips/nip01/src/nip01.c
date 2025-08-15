#include "nostr/nip01/nip01.h"
#include "nostr-tag.h"
#include "nostr-utils.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if defined(NOSTR_DEBUG) || defined(NOSTR_NIP01_DEBUG)
#include <stdio.h>
#define NIP01_DEBUGF(...) fprintf(stderr, __VA_ARGS__)
#else
#define NIP01_DEBUGF(...) do { } while (0)
#endif

static char *hex_from_32(const unsigned char bin[32]) {
    static const char *hex = "0123456789abcdef";
    char *out = (char *)malloc(65);
    if (!out) return NULL;
    for (size_t i = 0; i < 32; ++i) {
        out[2*i]   = hex[(bin[i] >> 4) & 0xF];
        out[2*i+1] = hex[bin[i] & 0xF];
    }
    out[64] = '\0';
    return out;
}

int nostr_nip01_add_e_tag(NostrEvent *ev,
                          const unsigned char event_id[32],
                          const char *relay_opt,
                          const unsigned char *author_pk /* optional */) {
    if (!ev || !event_id) return -EINVAL;
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) { tags = nostr_tags_new(0); nostr_event_set_tags(ev, tags); }

    char *id_hex = hex_from_32(event_id);
    if (!id_hex) return -ENOMEM;

    NostrTag *t = NULL;
    if (relay_opt && author_pk) {
        char *author_hex = hex_from_32(author_pk);
        if (!author_hex) { free(id_hex); return -ENOMEM; }
        t = nostr_tag_new("e", id_hex, relay_opt, author_hex, NULL);
        free(author_hex);
    } else if (relay_opt) {
        t = nostr_tag_new("e", id_hex, relay_opt, NULL);
    } else {
        t = nostr_tag_new("e", id_hex, NULL);
    }
    free(id_hex);
    if (!t) return -ENOMEM;
    nostr_tags_append(tags, t);
    return 0;
}

int nostr_nip01_add_p_tag(NostrEvent *ev,
                          const unsigned char pubkey[32],
                          const char *relay_opt) {
    if (!ev || !pubkey) return -EINVAL;
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) { tags = nostr_tags_new(0); nostr_event_set_tags(ev, tags); }

    char *pk_hex = hex_from_32(pubkey);
    if (!pk_hex) return -ENOMEM;
    NostrTag *t = relay_opt ? nostr_tag_new("p", pk_hex, relay_opt, NULL)
                            : nostr_tag_new("p", pk_hex, NULL);
    free(pk_hex);
    if (!t) return -ENOMEM;
    nostr_tags_append(tags, t);
    return 0;
}

int nostr_nip01_add_a_tag(NostrEvent *ev,
                          uint32_t kind,
                          const unsigned char pubkey[32],
                          const char *d_tag_opt,
                          const char *relay_opt) {
    if (!ev || !pubkey) return -EINVAL;
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) { tags = nostr_tags_new(0); nostr_event_set_tags(ev, tags); }

    char *pk_hex = hex_from_32(pubkey);
    if (!pk_hex) return -ENOMEM;

    /* Build "kind:pubkey[:d]" */
    size_t ref_len = 10 + 64 + (d_tag_opt ? strlen(d_tag_opt) + 1 : 0);
    char kind_buf[12];
    snprintf(kind_buf, sizeof(kind_buf), "%u", kind);
    ref_len = strlen(kind_buf) + 1 + 64 + (d_tag_opt ? strlen(d_tag_opt) + 1 : 0) + 1;
    char *ref = (char *)malloc(ref_len);
    if (!ref) { free(pk_hex); return -ENOMEM; }
    if (d_tag_opt) {
        snprintf(ref, ref_len, "%s:%s:%s", kind_buf, pk_hex, d_tag_opt);
    } else {
        snprintf(ref, ref_len, "%s:%s", kind_buf, pk_hex);
    }

    NostrTag *t = relay_opt ? nostr_tag_new("a", ref, relay_opt, NULL)
                            : nostr_tag_new("a", ref, NULL);
    free(pk_hex);
    free(ref);
    if (!t) return -ENOMEM;
    nostr_tags_append(tags, t);
    return 0;
}

int nostr_nip01_get_alt(const NostrEvent *ev, char **out_alt) {
    if (out_alt) *out_alt = NULL;
    if (!ev || !out_alt) return -EINVAL;
    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return -ENOENT;
    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;
        const char *k = nostr_tag_get(t, 0);
        if (k && strcmp(k, "alt") == 0) {
            const char *v = nostr_tag_get(t, 1);
            if (!v) continue;
            char *dup = (char *)malloc(strlen(v) + 1);
            if (!dup) return -ENOMEM;
            strcpy(dup, v);
            *out_alt = dup;
            return 0;
        }
    }
    return -ENOENT;
}

bool nostr_nip01_is_replaceable(int kind) {
    return kind == 0 || kind == 3 || (kind >= 10000 && kind < 20000);
}

bool nostr_nip01_is_addressable(int kind) {
    return (kind >= 30000 && kind < 40000);
}

bool nostr_nip01_is_ephemeral(int kind) {
    return (kind >= 20000 && kind < 30000);
}

/* --- Filter Builder --- */

typedef struct {
    uint64_t magic;
    NostrFilter *f;
} FBImpl;

#define FB_MAGIC 0x4642494D504C3031ULL /* 'FBIMPL01' */

int nostr_nip01_filter_builder_init(NostrFilterBuilder *fb) {
    if (!fb) return -EINVAL;
    FBImpl *impl = (FBImpl *)calloc(1, sizeof(FBImpl));
    if (!impl) return -ENOMEM;
    impl->magic = FB_MAGIC;
    impl->f = nostr_filter_new();
    if (!impl->f) { free(impl); return -ENOMEM; }
    fb->impl = impl;
    NIP01_DEBUGF("[nip01] fb_init impl=%p magic=%llx f=%p\n", (void*)impl, (unsigned long long)impl->magic, (void*)impl->f);
    return 0;
}

void nostr_nip01_filter_builder_dispose(NostrFilterBuilder *fb) {
    if (!fb || !fb->impl) return;
    FBImpl *impl = (FBImpl *)fb->impl;
    NIP01_DEBUGF("[nip01] fb_dispose impl=%p magic=%llx f=%p\n", (void*)impl, (unsigned long long)impl->magic, (void*)impl->f);
    if (impl->magic != FB_MAGIC) { fb->impl = NULL; return; }
    if (impl->f) nostr_filter_free(impl->f);
    impl->f = NULL;
    impl->magic = 0;
    free(impl);
    fb->impl = NULL;
}

static int fb_check(NostrFilterBuilder *fb) {
    if (!fb || !fb->impl) return -EINVAL;
    FBImpl *impl = (FBImpl *)fb->impl;
    return (impl->magic == FB_MAGIC) ? 0 : -EINVAL;
}

int nostr_nip01_filter_by_ids(NostrFilterBuilder *fb, const unsigned char (*ids)[32], size_t n) {
    if (fb_check(fb) < 0) return -EINVAL;
    FBImpl *impl = (FBImpl *)fb->impl;
    for (size_t i = 0; i < n; ++i) {
        char *hex = hex_from_32(ids[i]); if (!hex) return -ENOMEM;
        nostr_filter_add_id(impl->f, hex);
        free(hex);
    }
    return 0;
}

int nostr_nip01_filter_by_authors(NostrFilterBuilder *fb, const unsigned char (*pubkeys)[32], size_t n) {
    if (fb_check(fb) < 0) return -EINVAL;
    FBImpl *impl = (FBImpl *)fb->impl;
    for (size_t i = 0; i < n; ++i) {
        char *hex = hex_from_32(pubkeys[i]); if (!hex) return -ENOMEM;
        nostr_filter_add_author(impl->f, hex);
        free(hex);
    }
    return 0;
}

int nostr_nip01_filter_by_kinds(NostrFilterBuilder *fb, const int *kinds, size_t n) {
    if (fb_check(fb) < 0) return -EINVAL;
    FBImpl *impl = (FBImpl *)fb->impl;
    for (size_t i = 0; i < n; ++i) nostr_filter_add_kind(impl->f, kinds[i]);
    return 0;
}

int nostr_nip01_filter_by_tag_e(NostrFilterBuilder *fb, const unsigned char (*ids)[32], size_t n) {
    if (fb_check(fb) < 0) return -EINVAL;
    FBImpl *impl = (FBImpl *)fb->impl;
    for (size_t i = 0; i < n; ++i) {
        char *hex = hex_from_32(ids[i]); if (!hex) return -ENOMEM;
        nostr_filter_tags_append(impl->f, "e", hex, NULL);
        free(hex);
    }
    return 0;
}

int nostr_nip01_filter_by_tag_p(NostrFilterBuilder *fb, const unsigned char (*pubkeys)[32], size_t n) {
    if (fb_check(fb) < 0) return -EINVAL;
    FBImpl *impl = (FBImpl *)fb->impl;
    for (size_t i = 0; i < n; ++i) {
        char *hex = hex_from_32(pubkeys[i]); if (!hex) return -ENOMEM;
        nostr_filter_tags_append(impl->f, "p", hex, NULL);
        free(hex);
    }
    return 0;
}

int nostr_nip01_filter_by_tag_a(NostrFilterBuilder *fb, const char **a_refs, size_t n) {
    if (fb_check(fb) < 0) return -EINVAL;
    FBImpl *impl = (FBImpl *)fb->impl;
    for (size_t i = 0; i < n; ++i) {
        nostr_filter_tags_append(impl->f, "a", a_refs[i], NULL);
    }
    return 0;
}

int nostr_nip01_filter_since(NostrFilterBuilder *fb, uint32_t since) {
    if (fb_check(fb) < 0) return -EINVAL;
    FBImpl *impl = (FBImpl *)fb->impl;
    nostr_filter_set_since_i64(impl->f, (int64_t)since);
    return 0;
}

int nostr_nip01_filter_until(NostrFilterBuilder *fb, uint32_t until) {
    if (fb_check(fb) < 0) return -EINVAL;
    FBImpl *impl = (FBImpl *)fb->impl;
    nostr_filter_set_until_i64(impl->f, (int64_t)until);
    return 0;
}

int nostr_nip01_filter_limit(NostrFilterBuilder *fb, int limit) {
    if (fb_check(fb) < 0) return -EINVAL;
    FBImpl *impl = (FBImpl *)fb->impl;
    nostr_filter_set_limit(impl->f, limit);
    return 0;
}

int nostr_nip01_filter_build(NostrFilterBuilder *fb, NostrFilter *out) {
    if (fb_check(fb) < 0 || !out) return -EINVAL;
    FBImpl *impl = (FBImpl *)fb->impl;
    /* Transfer ownership of the current filter struct into out. */
    NostrFilter *cur = impl->f;
    if (!cur) return -EINVAL;
    NIP01_DEBUGF("[nip01] fb_build before transfer impl=%p magic=%llx f=%p -> out=%p\n", (void*)impl, (unsigned long long)impl->magic, (void*)impl->f, (void*)out);
    *out = *cur; /* out now owns all internal pointers from cur */
    /* Mark builder as one-shot: clear pointer so dispose is a no-op for filter. */
    impl->f = NULL;
    /* Free just the old shell struct; its internals are now owned by out. */
    free(cur);
    NIP01_DEBUGF("[nip01] fb_build after transfer impl=%p magic=%llx f=%p\n", (void*)impl, (unsigned long long)impl->magic, (void*)impl->f);
    return 0;
}
