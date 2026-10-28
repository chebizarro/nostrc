/* Real Smart HTTP download followed by authenticated, EOSE-complete relay state. */
#include "hanami/hanami-grasp.h"
#include "hanami_test_grasp_metadata.h"
#include "nostr-event.h"
#include "keys.h"
#include <nip34.h>
#include <git2.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *sk = "0000000000000000000000000000000000000000000000000000000000000001";
static const char *maintainer_sk = "0000000000000000000000000000000000000000000000000000000000000002";
static const char *npub = "npub10xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqpkge6d";

static hanami_error_t sign_auth(const char *json, char **out, void *data)
{
    assert(data == (void *)sk);
    NostrEvent *event = nostr_event_new();
    assert(event && nostr_event_deserialize_compact(event, json, NULL) == 1);
    assert(nostr_event_sign(event, sk) == 0);
    *out = nostr_event_serialize_compact(event);
    nostr_event_free(event);
    return *out ? HANAMI_OK : HANAMI_ERR_NOMEM;
}

int main(int argc, char **argv)
{
    assert(argc == 4 || argc == 6);
    git_libgit2_init();
    if (!strcmp(argv[1], "sign")) {
        const char *tip = (!strcmp(argv[3], "mismatch") || !strcmp(argv[3], "owner-old"))
            ? "0000000000000000000000000000000000000000" : argv[2];
        nip34_ref_t refs[] = {{"refs/heads/main", (char *)tip},
                              {"refs/heads/ghost", (char *)tip}};
        bool announcement = !strcmp(argv[3], "announcement") ||
                            !strcmp(argv[3], "forged-announcement");
        char *maintainer_pk = nostr_key_get_public(maintainer_sk);
        assert(maintainer_pk);
        maintainer_pk[64] = '\0';
        const char *maintainers[] = {maintainer_pk, NULL};
        NostrEvent *event = announcement
            ? nip34_create_repo_announcement("repo", "repo", NULL, NULL, NULL,
                NULL, maintainers)
            : nip34_create_repo_state("repo", refs,
                !strcmp(argv[3], "missing") ? 2 : 1, "ref: refs/heads/main");
        free(maintainer_pk);
        assert(event);
        if (!strcmp(argv[3], "owner-old")) event->created_at -= 5;
        if (!strcmp(argv[3], "oversize")) {
            char payload[66000];
            memset(payload, 'x', sizeof(payload) - 1);
            payload[sizeof(payload) - 1] = '\0';
            nostr_event_set_content(event, payload);
        }
        const char *signer_sk = (!strcmp(argv[3], "maintainer") ||
            !strcmp(argv[3], "forged") || !strcmp(argv[3], "forged-announcement"))
            ? maintainer_sk : sk;
        char *pk = nostr_key_get_public(signer_sk);
        assert(pk);
        char xonly[65];
        memcpy(xonly, pk, 64);
        xonly[64] = '\0';
        nostr_event_set_pubkey(event, xonly);
        free(pk);
        assert(nostr_event_sign(event, signer_sk) == 0);
        char *json = nostr_event_serialize_compact(event);
        assert(json);
        puts(json);
        free(json);
        nostr_event_free(event);
        git_libgit2_shutdown();
        return 0;
    }
    assert(argc == 6 && !strcmp(argv[1], "fetch"));
    const char *scenario = argv[2];
    git_repository *repo = NULL;
    assert(git_repository_init(&repo, argv[3], 0) == 0);
    git_oid sentinel, fetched;
    assert(git_blob_create_frombuffer(&sentinel, repo, "sentinel", 8) == 0);
    assert(git_oid_fromstr(&fetched, argv[5]) == 0);
    git_reference *tracking = NULL;
    assert(git_reference_create(&tracking, repo, "refs/remotes/grasp/heads/main",
        &sentinel, 0, "fixture sentinel") == 0);
    git_reference_free(tracking);
    char relay[256], clone[512];
    assert(snprintf(relay, sizeof(relay), "ws://%s/", argv[4]) > 0);
    assert(snprintf(clone, sizeof(clone), "http://%s/%s/repo.git", argv[4], npub) > 0);
    const char *relays[] = {relay, NULL};
    const char *too_many[] = {relay, relay, relay, relay, relay,
                              relay, relay, relay, relay, NULL};
    assert(hanami_grasp_fetch(repo, clone, too_many, 9) == HANAMI_ERR_INVALID_ARG);
    char *pk = nostr_key_get_public(sk);
    assert(pk);
    pk[64] = '\0';
    hanami_signer_t signer = {.pubkey = pk, .sign = sign_auth, .user_data = (void *)sk};
    hanami_grasp_metadata_t *metadata = NULL;
    bool info_only = !strcmp(scenario, "info");
    hanami_error_t result = hanami_grasp_fetch_with_result(info_only ? NULL : repo,
        clone, relays, 1, !strcmp(scenario, "auth-success") ? &signer : NULL, &metadata);
    if (info_only) {
        assert(hanami_grasp_fetch(NULL, clone, relays, 1) == HANAMI_OK);
        assert(hanami_grasp_fetch_with_signer(NULL, clone, relays, 1, NULL) == HANAMI_OK);
    }
    check_metadata(scenario, metadata);
    hanami_grasp_metadata_free(metadata);
    hanami_error_t expected = (!strcmp(scenario, "match") ||
        !strcmp(scenario, "maintainer") || !strcmp(scenario, "auth-success") ||
        !strcmp(scenario, "nip11-no-curation") || info_only) ? HANAMI_OK
        : !strcmp(scenario, "none") ? HANAMI_ERR_NOT_FOUND
        : !strcmp(scenario, "auth") ? HANAMI_ERR_AUTH : HANAMI_ERR_NOSTR;
    if (nip11_expected(scenario) != HANAMI_OK) expected = nip11_expected(scenario);
    fprintf(stderr, "fetch %s: result=%d expected=%d\n", scenario, result, expected);
    assert(result == expected);
    assert(git_reference_lookup(&tracking, repo, "refs/remotes/grasp/heads/main") == 0);
    assert(git_oid_cmp(git_reference_target(tracking),
        result == HANAMI_OK && !info_only ? &fetched : &sentinel) == 0);
    git_reference_free(tracking);
    if (result == HANAMI_OK && !info_only) {
        git_odb *odb = NULL;
        assert(git_repository_odb(&odb, repo) == 0);
        assert(git_odb_exists(odb, &fetched));
        git_odb_free(odb);
    }
    free(pk);
    git_repository_free(repo);
    git_libgit2_shutdown();
    return 0;
}
