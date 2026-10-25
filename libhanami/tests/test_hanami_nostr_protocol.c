/* Loopback wire tests of the public Hanami entrypoints and libnostr pool. */
#include "hanami/hanami-nostr.h"
#include "nostr-event.h"
#include "keys.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *sk = "0000000000000000000000000000000000000000000000000000000000000001";
static const char *pk;

static hanami_error_t sign_event(const char *json, char **out, void *data)
{
    (void)data;
    NostrEvent *event = nostr_event_new();
    assert(event);
    assert(nostr_event_deserialize_compact(event, json, NULL) == 1);
    assert(nostr_event_sign(event, sk) == 0);
    *out = nostr_event_serialize_compact(event);
    nostr_event_free(event);
    return *out ? HANAMI_OK : HANAMI_ERR_NOMEM;
}

static hanami_nostr_ctx_t *context(const char *base, const char *mode)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/%s", base, mode);
    const char *urls[] = { url, NULL };
    hanami_signer_t signer = { .pubkey = pk, .sign = sign_event };
    hanami_nostr_ctx_t *ctx = NULL;
    assert(hanami_nostr_ctx_new(urls, &signer, &ctx) == HANAMI_OK);
    return ctx;
}

static void publish_state(hanami_nostr_ctx_t *ctx, const char *target, int age)
{
    nip34_ref_t ref = { .refname = "refs/heads/main", .target = (char *)target };
    NostrEvent *event = nip34_create_repo_state("repo", &ref, 1, "ref: refs/heads/main");
    assert(event);
    nostr_event_set_pubkey(event, pk);
    event->created_at = time(NULL) - age;
    assert(nostr_event_sign(event, sk) == 0);
    assert(hanami_nostr_publish_event(ctx, event) == HANAMI_OK);
    nostr_event_free(event);
}

static void expect_fetch(hanami_nostr_ctx_t *ctx, hanami_error_t expected,
                         const char *target)
{
    nip34_repo_state_t *state = NULL;
    hanami_error_t result = hanami_nostr_fetch_state(ctx, "repo", pk, &state);
    if (result != expected)
        fprintf(stderr, "fetch: expected %d, got %d\n", expected, result);
    assert(result == expected);
    if (target) {
        assert(state && state->ref_count == 1);
        assert(strcmp(state->refs[0].target, target) == 0);
    } else {
        assert(state == NULL);
    }
    nip34_repo_state_free(state);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    char *pubkey = nostr_key_get_public(sk);
    assert(pubkey);
    pk = pubkey;
    const char *old_oid = "1111111111111111111111111111111111111111";
    const char *new_oid = "2222222222222222222222222222222222222222";
    hanami_nostr_ctx_t *old = context(argv[1], "old");
    hanami_nostr_ctx_t *new = context(argv[1], "new");
    publish_state(old, old_oid, 100);
    publish_state(new, new_oid, 90);

    char old_url[256], new_url[256];
    snprintf(old_url, sizeof(old_url), "%s/old", argv[1]);
    snprintf(new_url, sizeof(new_url), "%s/new", argv[1]);
    const char *urls[] = { old_url, new_url, old_url, NULL };
    hanami_nostr_ctx_t *combined = NULL;
    assert(hanami_nostr_ctx_new(urls, NULL, &combined) == HANAMI_OK);
    expect_fetch(combined, HANAMI_OK, new_oid);
    /* A repeated snapshot must not disappear into a pool-wide seen-ID cache. */
    expect_fetch(combined, HANAMI_OK, new_oid);
    hanami_nostr_ctx_free(combined);
    hanami_nostr_ctx_free(old);
    hanami_nostr_ctx_free(new);

    hanami_nostr_ctx_t *ctx = context(argv[1], "repo");
    assert(hanami_nostr_publish_repo(ctx, "repo", "Wire test", NULL, NULL, NULL) == HANAMI_OK);
    nip34_repository_t *repo = NULL;
    assert(hanami_nostr_fetch_repo(ctx, "repo", pk, &repo) == HANAMI_OK);
    assert(repo && strcmp(repo->name, "Wire test") == 0);
    nip34_repository_free(repo);
    hanami_nostr_ctx_free(ctx);

    ctx = context(argv[1], "state");
    nip34_ref_t ref = { .refname = "refs/heads/main", .target = (char *)new_oid };
    assert(hanami_nostr_publish_state(ctx, "repo", &ref, 1, NULL) == HANAMI_OK);
    expect_fetch(ctx, HANAMI_OK, new_oid);
    hanami_nostr_ctx_free(ctx);

    ctx = context(argv[1], "empty");
    expect_fetch(ctx, HANAMI_ERR_NOT_FOUND, NULL);
    hanami_nostr_ctx_free(ctx);
    ctx = context(argv[1], "closed");
    expect_fetch(ctx, HANAMI_ERR_NOSTR, NULL);
    hanami_nostr_ctx_free(ctx);
    ctx = context(argv[1], "auth");
    expect_fetch(ctx, HANAMI_ERR_AUTH, NULL);
    hanami_nostr_ctx_free(ctx);
    ctx = context(argv[1], "reject");
    assert(hanami_nostr_publish_state(ctx, "repo", &ref, 1, NULL) == HANAMI_ERR_NOSTR);
    hanami_nostr_ctx_free(ctx);
    ctx = context(argv[1], "publish-auth");
    assert(hanami_nostr_publish_state(ctx, "repo", &ref, 1, NULL) == HANAMI_ERR_AUTH);
    hanami_nostr_ctx_free(ctx);
    ctx = context(argv[1], "invalid");
    publish_state(ctx, new_oid, 0);
    expect_fetch(ctx, HANAMI_ERR_NOT_FOUND, NULL);
    hanami_nostr_ctx_free(ctx);
    ctx = context("invalid://fixture", "network");
    expect_fetch(ctx, HANAMI_ERR_NETWORK, NULL);
    assert(hanami_nostr_publish_state(ctx, "repo", &ref, 1, NULL) == HANAMI_ERR_NETWORK);
    hanami_nostr_ctx_free(ctx);
    ctx = context(argv[1], "no-eose");
    publish_state(ctx, new_oid, 0);
    /* An EVENT without EOSE is not a completed snapshot. */
    expect_fetch(ctx, HANAMI_ERR_TIMEOUT, NULL);
    hanami_nostr_ctx_free(ctx);

    free(pubkey);
    puts("Hanami pool wire protocol tests passed");
    return 0;
}
