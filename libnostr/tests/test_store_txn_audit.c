/* Regression tests for Item C nostrdb transaction scoping audit fixes.
 *
 * Covers:
 *   nostrc-mrj  TLS transaction cache and liveness are scoped by struct ndb *
 */
#define _XOPEN_SOURCE 700
#include <assert.h>
#include <ftw.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "libnostr_store.h"

/* Exported by the nostrdb backend for shutdown/cache maintenance. */
void ln_ndb_force_close_txn_cache(void);

/* ndb_backend.c optionally asks the nostr-gobject storage layer for a
 * subscription callback. This standalone libnostr test does not use one. */
typedef void (*storage_ndb_notify_fn)(void *ctx, uint64_t subid);
void storage_ndb_get_notify_callback(storage_ndb_notify_fn *fn_out, void **ctx_out)
{
    if (fn_out) *fn_out = NULL;
    if (ctx_out) *ctx_out = NULL;
}

static int rm_tree_cb(const char *path, const struct stat *st, int typeflag, struct FTW *ftwbuf)
{
    (void)st;
    (void)typeflag;
    (void)ftwbuf;
    return remove(path);
}

static void rm_tree(const char *path)
{
    if (path && *path) (void)nftw(path, rm_tree_cb, 64, FTW_DEPTH | FTW_PHYS);
}

static void free_query_results(void *results, int count)
{
    char **arr = (char **)results;
    if (!arr) return;
    for (int i = 0; i < count; i++) free(arr[i]);
    free(arr);
}

static int query_contains(ln_store *store, const char *needle, const char *forbidden)
{
    void *txn = NULL;
    void *results = NULL;
    int count = 0;
    int found = 0;

    assert(ln_store_begin_query(store, &txn) == LN_OK);
    assert(txn != NULL);
    assert(ln_store_query(store, txn, "{\"kinds\":[1]}", &results, &count) == LN_OK);

    char **arr = (char **)results;
    for (int i = 0; i < count; i++) {
        if (!arr[i]) continue;
        if (forbidden) assert(strstr(arr[i], forbidden) == NULL);
        if (needle && strstr(arr[i], needle) != NULL) found = 1;
    }

    free_query_results(results, count);
    assert(ln_store_end_query(store, txn) == LN_OK);
    return found;
}

static void wait_until_visible(ln_store *store, const char *needle)
{
    const struct timespec delay = { .tv_sec = 0, .tv_nsec = 100 * 1000 * 1000 };
    for (int i = 0; i < 50; i++) {
        if (query_contains(store, needle, NULL)) return;
        /* Do not let an empty pre-commit snapshot stay cached while polling. */
        ln_ndb_force_close_txn_cache();
        nanosleep(&delay, NULL);
    }
    assert(!"event did not become visible in nostrdb store");
}

static void test_two_store_tls_txn_scoping(void)
{
    char dir_a[128];
    char dir_b[128];
    snprintf(dir_a, sizeof(dir_a), "/tmp/nostrc-ndb-a-%ld", (long)getpid());
    snprintf(dir_b, sizeof(dir_b), "/tmp/nostrc-ndb-b-%ld", (long)getpid());
    rm_tree(dir_a);
    rm_tree(dir_b);
    assert(mkdir(dir_a, 0700) == 0);
    assert(mkdir(dir_b, 0700) == 0);

    assert(setenv("NOSTR_ALLOW_UNSAFE_INGEST", "1", 1) == 0);

    ln_store *store_a = NULL;
    ln_store *store_b = NULL;
    const char *opts = "{\"ingest_skip_validation\":1,\"ingester_threads\":1}";
    assert(ln_store_open("nostrdb", dir_a, opts, &store_a) == LN_OK);
    assert(ln_store_open("nostrdb", dir_b, opts, &store_b) == LN_OK);

    const char *event_a =
        "{\"id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "\"pubkey\":\"1111111111111111111111111111111111111111111111111111111111111111\","
        "\"created_at\":101,\"kind\":1,\"tags\":[],\"content\":\"store-a-only\","
        "\"sig\":\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
                 "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\"}";
    const char *event_b =
        "{\"id\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
        "\"pubkey\":\"2222222222222222222222222222222222222222222222222222222222222222\","
        "\"created_at\":202,\"kind\":1,\"tags\":[],\"content\":\"store-b-only\","
        "\"sig\":\"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
                 "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\"}";

    assert(ln_store_ingest_event_json(store_a, event_a, -1, NULL) == LN_OK);
    assert(ln_store_ingest_event_json(store_b, event_b, -1, NULL) == LN_OK);

    wait_until_visible(store_a, "store-a-only");
    wait_until_visible(store_b, "store-b-only");

    /* Clear polling txns, then intentionally query A followed by B within the
     * two-second reuse window. Pre-fix, B reused A's cached txn and returned
     * A's data. */
    ln_ndb_force_close_txn_cache();
    assert(query_contains(store_a, "store-a-only", "store-b-only") == 1);
    assert(query_contains(store_b, "store-b-only", "store-a-only") == 1);

    ln_ndb_force_close_txn_cache();
    ln_store_close(store_a);
    ln_store_close(store_b);
    rm_tree(dir_a);
    rm_tree(dir_b);
    printf("  [ok] nostrdb TLS txn cache is scoped per store handle\n");
}

int main(void)
{
    printf("libnostr store txn audit regression tests:\n");
    test_two_store_tls_txn_scoping();
    printf("All libnostr store txn audit tests passed.\n");
    return 0;
}
