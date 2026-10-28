/* Loopback wire tests of the public Hanami entrypoints and libnostr pool. */
#include "hanami/hanami-nostr.h"
#include "nostr-event.h"
#include "keys.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

static const char *sk = "0000000000000000000000000000000000000000000000000000000000000001";
static const char *pk;
static pthread_t caller_thread;

static hanami_error_t sign_event(const char *json, char **out, void *data)
{
    assert(data == (void *)sk);
    assert(pthread_equal(pthread_self(), caller_thread));
    NostrEvent *event = nostr_event_new();
    assert(event);
    assert(nostr_event_deserialize_compact(event, json, NULL) == 1);
    nostr_event_set_pubkey(event, pk);
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
    hanami_signer_t signer = { .pubkey = pk, .sign = sign_event, .user_data = (void *)sk };
    hanami_nostr_ctx_t *ctx = NULL;
    assert(hanami_nostr_ctx_new(urls, !strcmp(mode, "auth") ? NULL : &signer, &ctx) == HANAMI_OK);
    return ctx;
}

/* Two fixture relays in one context; a NULL signer cannot answer AUTH. */
static hanami_nostr_ctx_t *pair_context(const char *base, const char *a, const char *b,
                                        bool with_signer)
{
    char url_a[256], url_b[256];
    snprintf(url_a, sizeof(url_a), "%s/%s", base, a);
    snprintf(url_b, sizeof(url_b), "%s/%s", base, b);
    const char *urls[] = { url_a, url_b, NULL };
    hanami_signer_t signer = { .pubkey = pk, .sign = sign_event, .user_data = (void *)sk };
    hanami_nostr_ctx_t *ctx = NULL;
    assert(hanami_nostr_ctx_new(urls, with_signer ? &signer : NULL, &ctx) == HANAMI_OK);
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
    caller_thread = pthread_self();
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
    ctx = context(argv[1], "restricted");
    expect_fetch(ctx, HANAMI_ERR_AUTH, NULL);
    hanami_nostr_ctx_free(ctx);
    /* One relay's CLOSED is that relay's failure: the snapshot completes on
     * the other relay's EOSE, in either order of subscription. */
    ctx = pair_context(argv[1], "closed", "state", true);
    expect_fetch(ctx, HANAMI_OK, new_oid);
    hanami_nostr_ctx_free(ctx);
    ctx = pair_context(argv[1], "state", "restricted", true);
    expect_fetch(ctx, HANAMI_OK, new_oid);
    hanami_nostr_ctx_free(ctx);
    /* Unanswerable AUTH (no signer) does not abort the other relay. */
    ctx = pair_context(argv[1], "auth", "state", false);
    expect_fetch(ctx, HANAMI_OK, new_oid);
    hanami_nostr_ctx_free(ctx);
    /* Absence is unproven while a relay refused: an error, not NOT_FOUND. */
    ctx = pair_context(argv[1], "closed", "empty", true);
    expect_fetch(ctx, HANAMI_ERR_NOSTR, NULL);
    hanami_nostr_ctx_free(ctx);
    /* nostrc-837ap: a relay that cannot be subscribed is unanswered, not
     * excluded. Absence is unproven (NETWORK); an event elsewhere still
     * wins; a refusal's classification outranks unreachability. */
    ctx = pair_context(argv[1], "unavailable", "empty", true);
    expect_fetch(ctx, HANAMI_ERR_NETWORK, NULL);
    hanami_nostr_ctx_free(ctx);
    ctx = pair_context(argv[1], "state", "unavailable", true);
    expect_fetch(ctx, HANAMI_OK, new_oid);
    hanami_nostr_ctx_free(ctx);
    ctx = pair_context(argv[1], "unavailable", "closed", true);
    expect_fetch(ctx, HANAMI_ERR_NOSTR, NULL);
    hanami_nostr_ctx_free(ctx);
    ctx = context(argv[1], "reject");
    assert(hanami_nostr_publish_state(ctx, "repo", &ref, 1, NULL) == HANAMI_ERR_NOSTR);
    hanami_nostr_ctx_free(ctx);
    ctx = context(argv[1], "publish-auth");
    assert(hanami_nostr_publish_state(ctx, "repo", &ref, 1, NULL) == HANAMI_OK);
    /* A completed challenge is not permission to AUTH repeatedly. The fixture
     * rejects a second AUTH on this same connection. */
    expect_fetch(ctx, HANAMI_OK, new_oid);
    assert(hanami_nostr_publish_state(ctx, "repo", &ref, 1, NULL) == HANAMI_OK);
    hanami_nostr_ctx_free(ctx);
    ctx = context(argv[1], "auth-late");
    assert(hanami_nostr_publish_state(ctx, "repo", &ref, 1, NULL) == HANAMI_OK);
    hanami_nostr_ctx_free(ctx);
    ctx = context(argv[1], "auth-next");
    assert(hanami_nostr_publish_state(ctx, "repo", &ref, 1, NULL) == HANAMI_OK);
    assert(hanami_nostr_publish_state(ctx, "repo", &ref, 1, NULL) == HANAMI_OK);
    hanami_nostr_ctx_free(ctx);
    ctx = context(argv[1], "auth-early");
    assert(hanami_nostr_publish_state(ctx, "repo", &ref, 1, NULL) == HANAMI_OK);
    hanami_nostr_ctx_free(ctx);
    ctx = context(argv[1], "auth-denied");
    assert(hanami_nostr_publish_state(ctx, "repo", &ref, 1, NULL) == HANAMI_ERR_AUTH);
    hanami_nostr_ctx_free(ctx);
    ctx = context(argv[1], "auth-repeat");
    assert(hanami_nostr_publish_state(ctx, "repo", &ref, 1, NULL) == HANAMI_ERR_AUTH);
    hanami_nostr_ctx_free(ctx);
    ctx = context(argv[1], "auth-no-ok");
    assert(hanami_nostr_publish_state(ctx, "repo", &ref, 1, NULL) == HANAMI_ERR_TIMEOUT);
    hanami_nostr_ctx_free(ctx);
    ctx = context(argv[1], "query-auth");
    publish_state(ctx, new_oid, 0);
    expect_fetch(ctx, HANAMI_OK, new_oid);
    hanami_nostr_ctx_free(ctx);
    const char *wrong_urls[] = {argv[1], NULL};
    hanami_signer_t wrong_signer = {.pubkey = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        .sign = sign_event, .user_data = (void *)sk};
    assert(hanami_nostr_ctx_new(wrong_urls, &wrong_signer, &ctx) == HANAMI_OK);
    assert(hanami_nostr_publish_state(ctx, "repo", &ref, 1, NULL) == HANAMI_ERR_AUTH);
    hanami_nostr_ctx_free(ctx);
    char reject_url[256], accept_url[256];
    snprintf(reject_url, sizeof(reject_url), "%s/reject", argv[1]);
    snprintf(accept_url, sizeof(accept_url), "%s/state", argv[1]);
    const char *mixed_urls[] = {reject_url, accept_url, NULL};
    hanami_signer_t signer = {.pubkey = pk, .sign = sign_event, .user_data = (void *)sk};
    assert(hanami_nostr_ctx_new(mixed_urls, &signer, &ctx) == HANAMI_OK);
    assert(hanami_nostr_publish_state(ctx, "repo", &ref, 1, NULL) == HANAMI_OK);
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
