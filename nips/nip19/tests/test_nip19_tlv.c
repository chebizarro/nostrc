#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "nip19.h"

static int test_nprofile_roundtrip(void) {
    NostrProfilePointer *p = nostr_profile_pointer_new();
    if (!p) return -1;
    p->public_key = strdup("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff");
    p->relays = NULL; p->relays_count = 0;
    const char *urls[] = { "wss://relay1.example", "wss://relay2.example" };
    for (size_t i = 0; i < 2; ++i) {
        char **nr = (char **)realloc(p->relays, (p->relays_count + 1) * sizeof(char *));
        if (!nr) { nostr_profile_pointer_free(p); return -1; }
        p->relays = nr; p->relays[p->relays_count++] = strdup(urls[i]);
    }
    char *bech = NULL; if (nostr_nip19_encode_nprofile(p, &bech) != 0) { nostr_profile_pointer_free(p); return -1; }
    NostrProfilePointer *q = NULL; if (nostr_nip19_decode_nprofile(bech, &q) != 0) { free(bech); nostr_profile_pointer_free(p); return -1; }
    int ok = (q && q->public_key && strcmp(q->public_key, p->public_key) == 0 && q->relays_count == p->relays_count);
    if (ok) {
        for (size_t i = 0; i < p->relays_count; ++i) {
            if (strcmp(q->relays[i], p->relays[i]) != 0) { ok = 0; break; }
        }
    }
    nostr_profile_pointer_free(p); nostr_profile_pointer_free(q); free(bech);
    return ok ? 0 : -1;
}

static int test_nevent_roundtrip(void) {
    NostrEventPointer *e = nostr_event_pointer_new(); if (!e) return -1;
    e->id = strdup("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    e->author = strdup("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    e->kind = 1;
    const char *urls[] = { "wss://relay.one", "wss://relay.two" };
    for (size_t i = 0; i < 2; ++i) {
        char **nr = (char **)realloc(e->relays, (e->relays_count + 1) * sizeof(char *));
        if (!nr) { nostr_event_pointer_free(e); return -1; }
        e->relays = nr; e->relays[e->relays_count++] = strdup(urls[i]);
    }
    char *bech = NULL; if (nostr_nip19_encode_nevent(e, &bech) != 0) { nostr_event_pointer_free(e); return -1; }
    NostrEventPointer *d = NULL; if (nostr_nip19_decode_nevent(bech, &d) != 0) { free(bech); nostr_event_pointer_free(e); return -1; }
    int ok = (d && d->id && strcmp(d->id, e->id) == 0 && d->author && strcmp(d->author, e->author) == 0 && d->kind == e->kind && d->relays_count == e->relays_count);
    if (ok) {
        for (size_t i = 0; i < e->relays_count; ++i) if (strcmp(d->relays[i], e->relays[i]) != 0) { ok = 0; break; }
    }
    nostr_event_pointer_free(e); nostr_event_pointer_free(d); free(bech);
    return ok ? 0 : -1;
}

static int test_naddr_roundtrip(void) {
    NostrEntityPointer *a = nostr_entity_pointer_new(); if (!a) return -1;
    a->identifier = strdup("note-42");
    a->public_key = strdup("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc");
    a->kind = 30000;
    const char *urls[] = { "wss://relay.alpha", "wss://relay.beta" };
    for (size_t i = 0; i < 2; ++i) {
        char **nr = (char **)realloc(a->relays, (a->relays_count + 1) * sizeof(char *));
        if (!nr) { nostr_entity_pointer_free(a); return -1; }
        a->relays = nr; a->relays[a->relays_count++] = strdup(urls[i]);
    }
    char *bech = NULL; if (nostr_nip19_encode_naddr(a, &bech) != 0) { nostr_entity_pointer_free(a); return -1; }
    NostrEntityPointer *b = NULL; if (nostr_nip19_decode_naddr(bech, &b) != 0) { free(bech); nostr_entity_pointer_free(a); return -1; }
    int ok = (b && b->identifier && strcmp(b->identifier, a->identifier) == 0 && b->public_key && strcmp(b->public_key, a->public_key) == 0 && b->kind == a->kind && b->relays_count == a->relays_count);
    if (ok) { for (size_t i = 0; i < a->relays_count; ++i) if (strcmp(b->relays[i], a->relays[i]) != 0) { ok = 0; break; } }
    nostr_entity_pointer_free(a); nostr_entity_pointer_free(b); free(bech); return ok ? 0 : -1;
}

static int test_nrelay_roundtrip(void) {
    const char *url = "wss://relay.example";
    char *bech = NULL; if (nostr_nip19_encode_nrelay(url, &bech) != 0) return -1;
    char **relays = NULL; size_t n = 0; if (nostr_nip19_decode_nrelay(bech, &relays, &n) != 0) { free(bech); return -1; }
    int ok = (n == 1 && relays && strcmp(relays[0], url) == 0);
    if (relays) { for (size_t i = 0; i < n; ++i) free(relays[i]); free(relays); }
    free(bech); return ok ? 0 : -1;
}

int main(void) {
    if (test_nprofile_roundtrip() != 0) { fprintf(stderr, "nprofile roundtrip failed\n"); return 1; }
    if (test_nevent_roundtrip() != 0) { fprintf(stderr, "nevent roundtrip failed\n"); return 1; }
    if (test_naddr_roundtrip() != 0) { fprintf(stderr, "naddr roundtrip failed\n"); return 1; }
    if (test_nrelay_roundtrip() != 0) { fprintf(stderr, "nrelay roundtrip failed\n"); return 1; }
    printf("test_nip19_tlv: OK\n");
    return 0;
}
