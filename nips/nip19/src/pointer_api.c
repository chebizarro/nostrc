#include "nip19.h"
#include "nostr-pointer.h"
#include <stdlib.h>
#include <string.h>

static char *dupstr(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *d = (char *)malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static int copy_relays(char ***out, size_t *out_cnt, const char *const *relays, size_t relays_count) {
    *out = NULL; if (out_cnt) *out_cnt = 0;
    if (!relays || relays_count == 0) return 0;
    char **arr = (char **)malloc(relays_count * sizeof(char *));
    if (!arr) return -1;
    size_t i;
    for (i = 0; i < relays_count; ++i) {
        arr[i] = dupstr(relays[i]);
        if (!arr[i]) {
            for (size_t j = 0; j < i; ++j) free(arr[j]);
            free(arr);
            return -1;
        }
    }
    *out = arr; if (out_cnt) *out_cnt = relays_count; return 0;
}

void nostr_pointer_free(NostrPointer *ptr) {
    if (!ptr) return;
    switch (ptr->kind) {
        case NOSTR_PTR_NPROFILE:
            if (ptr->u.nprofile) nostr_profile_pointer_free(ptr->u.nprofile);
            break;
        case NOSTR_PTR_NEVENT:
            if (ptr->u.nevent) nostr_event_pointer_free(ptr->u.nevent);
            break;
        case NOSTR_PTR_NADDR:
            if (ptr->u.naddr) nostr_entity_pointer_free(ptr->u.naddr);
            break;
        case NOSTR_PTR_NRELAY:
            if (ptr->u.nrelay.relays) {
                for (size_t i = 0; i < ptr->u.nrelay.count; ++i) free(ptr->u.nrelay.relays[i]);
                free(ptr->u.nrelay.relays);
            }
            break;
        default: break;
    }
    free(ptr);
}

int nostr_pointer_parse(const char *bech, NostrPointer **out_ptr) {
    if (!bech || !out_ptr) return -1; *out_ptr = NULL;
    NostrBech32Type t;
    if (nostr_nip19_inspect(bech, &t) != 0) return -1;

    NostrPointer *P = (NostrPointer *)calloc(1, sizeof(NostrPointer));
    if (!P) return -1;

    int rc = -1;
    if (t == NOSTR_B32_NPROFILE) {
        NostrProfilePointer *p = NULL;
        if (nostr_nip19_decode_nprofile(bech, &p) != 0) goto done;
        P->kind = NOSTR_PTR_NPROFILE; P->u.nprofile = p; p = NULL; rc = 0;
    } else if (t == NOSTR_B32_NEVENT) {
        NostrEventPointer *e = NULL;
        if (nostr_nip19_decode_nevent(bech, &e) != 0) goto done;
        P->kind = NOSTR_PTR_NEVENT; P->u.nevent = e; e = NULL; rc = 0;
    } else if (t == NOSTR_B32_NADDR) {
        NostrEntityPointer *a = NULL;
        if (nostr_nip19_decode_naddr(bech, &a) != 0) goto done;
        P->kind = NOSTR_PTR_NADDR; P->u.naddr = a; a = NULL; rc = 0;
    } else if (t == NOSTR_B32_NRELAY) {
        char **relays = NULL; size_t cnt = 0;
        if (nostr_nip19_decode_nrelay(bech, &relays, &cnt) != 0) goto done;
        P->kind = NOSTR_PTR_NRELAY; P->u.nrelay.relays = relays; P->u.nrelay.count = cnt; rc = 0;
    } else {
        rc = -1;
    }

 done:
    if (rc != 0) { free(P); P = NULL; }
    *out_ptr = P; return rc;
}

int nostr_pointer_from_nprofile_config(const NostrNProfileConfig *cfg, NostrPointer **out_ptr) {
    if (!cfg || !out_ptr || !cfg->public_key) return -1; *out_ptr = NULL;
    NostrProfilePointer *p = nostr_profile_pointer_new(); if (!p) return -1;
    p->public_key = dupstr(cfg->public_key);
    if (!p->public_key) { nostr_profile_pointer_free(p); return -1; }
    if (copy_relays(&p->relays, &p->relays_count, cfg->relays, cfg->relays_count) != 0) { nostr_profile_pointer_free(p); return -1; }
    NostrPointer *P = (NostrPointer *)calloc(1, sizeof(NostrPointer)); if (!P) { nostr_profile_pointer_free(p); return -1; }
    P->kind = NOSTR_PTR_NPROFILE; P->u.nprofile = p; *out_ptr = P; return 0;
}

int nostr_pointer_from_nevent_config(const NostrNEventConfig *cfg, NostrPointer **out_ptr) {
    if (!cfg || !out_ptr || !cfg->id) return -1; *out_ptr = NULL;
    NostrEventPointer *e = nostr_event_pointer_new(); if (!e) return -1;
    e->id = dupstr(cfg->id);
    if (!e->id) { nostr_event_pointer_free(e); return -1; }
    if (cfg->author) { e->author = dupstr(cfg->author); if (!e->author) { nostr_event_pointer_free(e); return -1; } }
    e->kind = cfg->kind;
    if (copy_relays(&e->relays, &e->relays_count, cfg->relays, cfg->relays_count) != 0) { nostr_event_pointer_free(e); return -1; }
    NostrPointer *P = (NostrPointer *)calloc(1, sizeof(NostrPointer)); if (!P) { nostr_event_pointer_free(e); return -1; }
    P->kind = NOSTR_PTR_NEVENT; P->u.nevent = e; *out_ptr = P; return 0;
}

int nostr_pointer_from_naddr_config(const NostrNAddrConfig *cfg, NostrPointer **out_ptr) {
    if (!cfg || !out_ptr || !cfg->identifier || !cfg->public_key || cfg->kind <= 0) return -1; *out_ptr = NULL;
    NostrEntityPointer *a = nostr_entity_pointer_new(); if (!a) return -1;
    a->identifier = dupstr(cfg->identifier);
    a->public_key = dupstr(cfg->public_key);
    a->kind = cfg->kind;
    if (!a->identifier || !a->public_key) { nostr_entity_pointer_free(a); return -1; }
    if (copy_relays(&a->relays, &a->relays_count, cfg->relays, cfg->relays_count) != 0) { nostr_entity_pointer_free(a); return -1; }
    NostrPointer *P = (NostrPointer *)calloc(1, sizeof(NostrPointer)); if (!P) { nostr_entity_pointer_free(a); return -1; }
    P->kind = NOSTR_PTR_NADDR; P->u.naddr = a; *out_ptr = P; return 0;
}

int nostr_pointer_from_nrelay_config(const NostrNRelayConfig *cfg, NostrPointer **out_ptr) {
    if (!cfg || !out_ptr || !cfg->relays || cfg->relays_count == 0) return -1; *out_ptr = NULL;
    char **arr = NULL; size_t cnt = 0;
    if (copy_relays(&arr, &cnt, cfg->relays, cfg->relays_count) != 0) return -1;
    NostrPointer *P = (NostrPointer *)calloc(1, sizeof(NostrPointer)); if (!P) { for (size_t i=0;i<cnt;++i) free(arr[i]); free(arr); return -1; }
    P->kind = NOSTR_PTR_NRELAY; P->u.nrelay.relays = arr; P->u.nrelay.count = cnt; *out_ptr = P; return 0;
}

int nostr_pointer_to_bech32(const NostrPointer *ptr, char **out_bech) {
    if (!ptr || !out_bech) return -1; *out_bech = NULL;
    switch (ptr->kind) {
        case NOSTR_PTR_NPROFILE:
            return nostr_nip19_encode_nprofile(ptr->u.nprofile, out_bech);
        case NOSTR_PTR_NEVENT:
            return nostr_nip19_encode_nevent(ptr->u.nevent, out_bech);
        case NOSTR_PTR_NADDR:
            return nostr_nip19_encode_naddr(ptr->u.naddr, out_bech);
        case NOSTR_PTR_NRELAY:
            if (!ptr->u.nrelay.relays || ptr->u.nrelay.count == 0) return -1;
            return nostr_nip19_encode_nrelay_multi((const char *const *)ptr->u.nrelay.relays, ptr->u.nrelay.count, out_bech);
        default:
            return -1;
    }
}
