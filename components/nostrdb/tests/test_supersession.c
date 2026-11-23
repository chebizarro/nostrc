/* Real storage-vtable REQ/COUNT regressions; put_event confirms commit. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nostrdb_storage.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-json.h"
#include "nostr-keys.h"
#include "nostr-tag.h"

static const char *sk = "0000000000000000000000000000000000000000000000000000000000000001";
static char *pk;
static char *put(NostrStorage *st, const char *d, int64_t time, int serial) {
    NostrEvent *ev = nostr_event_new();
    char content[64]; snprintf(content, sizeof content, "version %d", serial);
    nostr_event_set_pubkey(ev, pk);
    nostr_event_set_kind(ev, 30023);
    nostr_event_set_created_at(ev, time);
    nostr_event_set_content(ev, content);
    if (d) nostr_event_set_tags(ev, nostr_tags_new(1, nostr_tag_new("d", d, NULL)));
    assert(nostr_event_sign(ev, sk) == 0);
    char *id = nostr_event_get_id(ev);
    assert(id && st->vt->put_event(st, ev) == 0);
    nostr_event_free(ev);
    return id;
}

static void query(NostrStorage *st, NostrFilter *f, const char *winner, unsigned expected) {
    int error = 0;
    void *it = st->vt->query(st, f, 1, 500, 0, 0, &error);
    assert(it && !error);
    unsigned got = 0, found = 0;
    for (;;) {
        NostrEvent *ev = nostr_event_new(); size_t n = 1;
        int rc = st->vt->query_next(st, it, ev, &n);
        assert(rc == 0); /* crowded coordinates must not abort unrelated results */
        if (!n) { nostr_event_free(ev); break; }
        char *id = nostr_event_get_id(ev);
        if (strcmp(id, winner) == 0) found++;
        free(id); nostr_event_free(ev); got++;
    }
    st->vt->query_free(st, it);
    assert(got == expected && found == (expected ? 1u : 0u));
    uint64_t count = 0;
    assert(st->vt->count(st, f, 1, &count) == 0 && count == expected);
}

int main(int argc, char **argv) {
    assert(argc == 3);
    nostr_json_init(); pk = nostr_key_get_public(sk); assert(pk);
    NostrStorage *st = nostrdb_storage_new(); assert(st);
    assert(st->vt->open(st, argv[1], NULL) == 0);
    NostrFilter *f = nostr_filter_new(); assert(f);
    nostr_filter_add_kind(f, 30023); f->limit = 500;
    char *winner = NULL;
    if (strcmp(argv[2], "crowded") == 0 || strcmp(argv[2], "increasing") == 0) {
        int increasing = strcmp(argv[2], "increasing") == 0;
        /* Same-time ties expose a capped candidate list even if its unseen
         * winner sorts after the chosen 256. The existence predicate handles
         * timestamp and canonical ID together, before applying limit 1. */
        for (int i = 0; i < 257; i++) {
            char *id = put(st, "crowded", 1700000000 + (increasing ? i : 0), i);
            if (increasing || !winner || strcmp(id, winner) < 0) { free(winner); winner = id; }
            else free(id);
        }
        free(put(st, "unrelated", 1700001000, 1000));
        query(st, f, winner, 2);
    } else {
        assert(strcmp(argv[2], "missing-d") == 0);
        winner = put(st, NULL, 1700000000, 0);
        /* These newer coordinates are NOT replacements of the missing-d
         * event, and must not turn its supersession probe into a cap error. */
        for (int i = 0; i < 257; i++) {
            char d[32]; snprintf(d, sizeof d, "other-%d", i);
            free(put(st, d, 1700000100 + i, i));
        }
        /* Force the missing-d candidate through both real iterator/count
         * paths, independent of the outer scan's unrelated capacity. */
        nostr_filter_add_id(f, winner);
        query(st, f, winner, 1);
        nostr_filter_free(f); f = nostr_filter_new();
        nostr_filter_add_kind(f, 30023); f->limit = 500;
        /* Empty and missing d share a coordinate, not any nonempty d. */
        char *empty = put(st, "", 1700001000, 999);
        nostr_filter_add_id(f, winner); query(st, f, winner, 0);
        nostr_filter_free(f); f = nostr_filter_new();
        free(winner); winner = empty;
        nostr_filter_add_id(f, winner); query(st, f, winner, 1);
        nostr_filter_free(f); f = nostr_filter_new();
        nostr_filter_add_id(f, winner);
        char *missing = put(st, NULL, 1700001001, 1000);
        query(st, f, winner, 0); /* a missing-d successor supersedes explicit empty d */
        nostr_filter_free(f); f = nostr_filter_new();
        nostr_filter_add_id(f, missing); query(st, f, missing, 1);
        free(missing);
        nostr_filter_free(f); f = nostr_filter_new();
        nostr_filter_add_kind(f, 30023); f->limit = 500;
        int error = 0; void *it = st->vt->query(st, f, 1, 500, 0, 0, &error);
        assert(it && !error);
        for (;;) {
            NostrEvent *ev = nostr_event_new(); size_t n = 1;
            assert(st->vt->query_next(st, it, ev, &n) == 0);
            nostr_event_free(ev); if (!n) break;
        }
        st->vt->query_free(st, it);
        uint64_t count = 0;
        assert(st->vt->count(st, f, 1, &count) == 0 && count == 258);
    }
    free(winner); nostr_filter_free(f); st->vt->close(st); free(st); free(pk);
    return 0;
}
