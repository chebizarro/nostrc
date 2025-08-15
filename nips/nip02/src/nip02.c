#include "nostr/nip02/nip02.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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

static int pubkey_eq_hex(const unsigned char a[32], const char *hex) {
    if (!hex || strlen(hex) != 64) return 0;
    for (size_t i=0;i<32;++i){
        unsigned char byte = 0;
        char c1 = hex[2*i], c2 = hex[2*i+1];
        int hi = (c1>='0'&&c1<='9')?c1-'0':(c1>='a'&&c1<='f')?10+(c1-'a'):(c1>='A'&&c1<='F')?10+(c1-'A'):-1;
        int lo = (c2>='0'&&c2<='9')?c2-'0':(c2>='a'&&c2<='f')?10+(c2-'a'):(c2>='A'&&c2<='F')?10+(c2-'A'):-1;
        if (hi<0||lo<0) return 0;
        byte = (unsigned char)((hi<<4)|lo);
        if (a[i] != byte) return 0;
    }
    return 1;
}

int nostr_nip02_build_follow_list(NostrEvent *ev,
                                  const unsigned char author_pk[32],
                                  const NostrFollowList *list,
                                  uint32_t created_at) {
    if (!ev || !author_pk || !list) return -EINVAL;
    /* Set kind and created_at */
    nostr_event_set_kind(ev, 3);
    nostr_event_set_created_at(ev, (int64_t)created_at);

    /* Set author pubkey string */
    char *author_hex = hex_from_32(author_pk);
    if (!author_hex) return -ENOMEM;
    nostr_event_set_pubkey(ev, author_hex);
    free(author_hex);

    /* Build tags from follow list */
    NostrTags *tags = nostr_tags_new(0);
    if (!tags) return -ENOMEM;
    for (size_t i=0;i<list->count;++i){
        const NostrFollowEntry *e = &list->entries[i];
        char *pk_hex = hex_from_32(e->pubkey);
        if (!pk_hex) { nostr_tags_free(tags); return -ENOMEM; }
        if (e->relay && *e->relay) {
            if (e->petname && *e->petname) {
                nostr_tags_append(tags, nostr_tag_new("p", pk_hex, e->relay, e->petname, NULL));
            } else {
                nostr_tags_append(tags, nostr_tag_new("p", pk_hex, e->relay, NULL));
            }
        } else {
            nostr_tags_append(tags, nostr_tag_new("p", pk_hex, NULL));
        }
        free(pk_hex);
    }
    nostr_event_set_tags(ev, tags);
    return 0;
}

int nostr_nip02_parse_follow_list(const NostrEvent *ev, NostrFollowList *out){
    if (!ev || !out) return -EINVAL;
    memset(out, 0, sizeof(*out));
    const NostrTags *tags = (const NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return 0; /* empty list */
    size_t n = nostr_tags_size(tags);
    out->entries = (NostrFollowEntry *)calloc(n, sizeof(NostrFollowEntry));
    if (!out->entries && n>0) return -ENOMEM;
    for (size_t i=0;i<n;++i){
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;
        const char *k = nostr_tag_get(t, 0);
        if (!k || strcmp(k, "p") != 0) continue;
        const char *pk_hex = nostr_tag_get(t, 1);
        if (!pk_hex) continue;
        NostrFollowEntry *e = &out->entries[out->count];
        /* decode hex */
        for (size_t j=0;j<32;++j){
            char c1 = pk_hex[2*j], c2 = pk_hex[2*j+1];
            int hi = (c1>='0'&&c1<='9')?c1-'0':(c1>='a'&&c1<='f')?10+(c1-'a'):(c1>='A'&&c1<='F')?10+(c1-'A'):-1;
            int lo = (c2>='0'&&c2<='9')?c2-'0':(c2>='a'&&c2<='f')?10+(c2-'a'):(c2>='A'&&c2<='F')?10+(c2-'A'):-1;
            if (hi<0||lo<0) { out->entries[out->count].relay=NULL; out->entries[out->count].petname=NULL; goto skip; }
            e->pubkey[j] = (unsigned char)((hi<<4)|lo);
        }
        if (nostr_tag_size(t) >= 3) {
            const char *relay = nostr_tag_get(t, 2);
            if (relay && *relay) e->relay = strdup(relay);
        }
        if (nostr_tag_size(t) >= 4) {
            const char *pet = nostr_tag_get(t, 3);
            if (pet && *pet) e->petname = strdup(pet);
        }
        out->count++;
        continue;
    skip:
        ;
    }
    return 0;
}

void nostr_nip02_free_follow_list(NostrFollowList *list){
    if (!list) return;
    for (size_t i=0;i<list->count;++i){
        free(list->entries[i].relay);
        free(list->entries[i].petname);
    }
    free(list->entries);
    list->entries=NULL; list->count=0;
}

int nostr_nip02_append(NostrEvent *ev, const NostrFollowEntry *add, size_t add_n){
    if (!ev || (!add && add_n>0)) return -EINVAL;
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) { tags = nostr_tags_new(0); nostr_event_set_tags(ev, tags); }
    size_t existing_n = nostr_tags_size(tags);

    for (size_t i=0;i<add_n;++i){
        const NostrFollowEntry *e = &add[i];
        int found = 0;
        for (size_t j=0;j<existing_n;++j){
            NostrTag *t = nostr_tags_get(tags, j);
            if (!t || nostr_tag_size(t) < 2) continue;
            const char *k = nostr_tag_get(t, 0);
            if (!k || strcmp(k, "p") != 0) continue;
            const char *pk_hex = nostr_tag_get(t, 1);
            if (pubkey_eq_hex(e->pubkey, pk_hex)) { found = 1; break; }
        }
        if (found) continue;
        char *pk_hex = hex_from_32(e->pubkey);
        if (!pk_hex) return -ENOMEM;
        if (e->relay && *e->relay) {
            if (e->petname && *e->petname) {
                nostr_tags_append(tags, nostr_tag_new("p", pk_hex, e->relay, e->petname, NULL));
            } else {
                nostr_tags_append(tags, nostr_tag_new("p", pk_hex, e->relay, NULL));
            }
        } else {
            nostr_tags_append(tags, nostr_tag_new("p", pk_hex, NULL));
        }
        free(pk_hex);
        existing_n = nostr_tags_size(tags); /* update */
    }
    return 0;
}
