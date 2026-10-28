/* End-to-end: signed relay OK -> libgit2 Smart HTTP -> corrective state. */
#include "hanami/hanami-grasp.h"
#include "hanami/hanami-nostr.h"
#include "nostr-event.h"
#include "keys.h"
#include <git2.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *sk = "0000000000000000000000000000000000000000000000000000000000000001";
static const char *npub = "npub10xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72k5cs2kd7pqf0h2zx";

static hanami_error_t sign_event(const char *json, char **out, void *data)
{
    assert(data == (void *)sk); /* Signer userdata must survive every layer. */
    NostrEvent *event = nostr_event_new();
    assert(event && nostr_event_deserialize_compact(event, json, NULL) == 1);
    assert(nostr_event_sign(event, sk) == 0);
    *out = nostr_event_serialize_compact(event);
    nostr_event_free(event);
    return *out ? HANAMI_OK : HANAMI_ERR_NOMEM;
}

static void commit(git_repository *repo, git_oid *oid, const git_oid *parent_id)
{
    git_treebuilder *builder = NULL;
    git_tree *tree = NULL;
    git_signature *sig = NULL;
    git_commit *parent = NULL;
    git_oid tree_id;
    assert(git_treebuilder_new(&builder, repo, NULL) == 0);
    assert(git_treebuilder_write(&tree_id, builder) == 0);
    assert(git_tree_lookup(&tree, repo, &tree_id) == 0);
    assert(git_signature_now(&sig, "fixture", "fixture@example.com") == 0);
    if (parent_id) assert(git_commit_lookup(&parent, repo, parent_id) == 0);
    const git_commit *parents[] = {parent};
    assert(git_commit_create(oid, repo, "refs/heads/main", sig, sig, NULL,
        parent ? "next" : "initial", tree, parent ? 1 : 0, parents) == 0);
    git_commit_free(parent);
    git_signature_free(sig);
    git_tree_free(tree);
    git_treebuilder_free(builder);
}

int main(int argc, char **argv)
{
    assert(argc == 4);
    git_libgit2_init();
    git_repository *repo = NULL;
    assert(git_repository_init(&repo, argv[3], 0) == 0);
    assert(git_repository_set_head(repo, "refs/heads/main") == 0);
    git_oid old, next;
    commit(repo, &old, NULL);
    commit(repo, &next, &old);
    char old_hex[GIT_OID_HEXSZ + 1];
    git_oid_tostr(old_hex, sizeof(old_hex), &old);
    char root[256], replica[256], clone[512];
    snprintf(root, sizeof(root), "ws://%s/", argv[1]);
    snprintf(replica, sizeof(replica), "ws://%s/replica", argv[1]);
    snprintf(clone, sizeof(clone), "http://%s/%s/repo.git", argv[1], npub);
    const char *roots[] = {root, NULL}, *replicas[] = {replica, NULL};
    char *pk = nostr_key_get_public(sk);
    assert(pk);
    hanami_signer_t signer = { .pubkey = pk, .sign = sign_event, .user_data = (void *)sk };
    if (strcmp(argv[2], "first-fail") && strcmp(argv[2], "closed")) {
        hanami_nostr_ctx_t *ctx = NULL;
        assert(hanami_nostr_ctx_new(roots, &signer, &ctx) == HANAMI_OK);
        nip34_ref_t refs[] = {{"refs/heads/main", old_hex}, {"refs/heads/unrelated", old_hex}};
        NostrEvent *event = nip34_create_repo_state("repo", refs, 2, "ref: refs/heads/main");
        assert(event);
        event->created_at = time(NULL) - 5;
        nostr_event_set_pubkey(event, pk);
        assert(nostr_event_sign(event, sk) == 0);
        assert(hanami_nostr_publish_event(ctx, event) == HANAMI_OK);
        nostr_event_free(event);
        hanami_nostr_ctx_free(ctx);
    }
    const char *specs[] = {"+refs/heads/main:refs/heads/main", NULL};
    hanami_grasp_push_opts_t opts = { .clone_url = clone, .relay_urls = replicas,
        .relay_count = 1, .signer = &signer, .repo_id = "repo",
        .refspecs = specs, .refspec_count = 1 };
    if (!strcmp(argv[2], "success")) { opts.refspecs = NULL; opts.refspec_count = 0; }
    hanami_grasp_push_result_t result;
    opts.repo_id = "another-repo";
    assert(hanami_push_to_grasp_with_result(repo, &opts, &result) == HANAMI_ERR_INVALID_ARG);
    assert(!result.state_accepted && !result.push_attempted);
    opts.repo_id = "repo";
    hanami_error_t err = hanami_push_to_grasp_with_result(repo, &opts, &result);
    fprintf(stderr, "%s: result=%d operation=%d recovery=%d git=%s\n", argv[2], err,
            result.operation_error, result.recovery_error,
            git_error_last() ? git_error_last()->message : "none");
    if (!strcmp(argv[2], "success") || !strcmp(argv[2], "auth-success")) {
        assert(err == HANAMI_OK && result.state_accepted && result.push_completed);
        assert(!result.recovery_attempted);
        const char *deletion[] = {":refs/heads/main", NULL};
        opts.refspecs = deletion;
        opts.refspec_count = 1;
        assert(hanami_push_to_grasp(repo, &opts) == HANAMI_OK);
    } else if (!strcmp(argv[2], "closed")) {
        assert(err == HANAMI_ERR_NOSTR && !result.state_accepted && !result.push_attempted);
    } else if (!strcmp(argv[2], "no-ok")) {
        assert(err == HANAMI_ERR_TIMEOUT && result.operation_error == HANAMI_ERR_TIMEOUT);
        assert(!result.state_accepted && !result.push_attempted);
        assert(result.recovery_attempted && result.recovery_error == HANAMI_OK);
    } else {
        assert(err != HANAMI_OK && !result.push_completed);
        assert(result.recovery_attempted);
        if (!strcmp(argv[2], "reject")) {
            assert(!result.state_accepted && !result.push_attempted);
        } else {
            assert(result.state_accepted && result.push_attempted);
            assert(result.operation_error == HANAMI_ERR_NETWORK);
            assert((result.recovery_error != HANAMI_OK) == !strcmp(argv[2], "correction-reject"));
        }
    }
    git_repository_free(repo);
    free(pk);
    git_libgit2_shutdown();
    return 0;
}
