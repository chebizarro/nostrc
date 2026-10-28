/* Real libgit2 ref operations: relay refusal must preserve committed refs. */
#include "hanami/hanami-refdb-backend.h"
#include "hanami/hanami-nostr.h"
#include "nostr-event.h"
#include "keys.h"
#include "hanami_test_fs.h"
#include <git2.h>
#include <git2/sys/refdb_backend.h>
#include <assert.h>

static const char *sk = "0000000000000000000000000000000000000000000000000000000000000001";
static hanami_error_t sign_event(const char *json, char **out, void *data)
{
    (void)data;
    NostrEvent *event = nostr_event_new();
    assert(event && nostr_event_deserialize_compact(event, json, NULL) == 1);
    assert(nostr_event_sign(event, sk) == 0);
    *out = nostr_event_serialize_compact(event);
    nostr_event_free(event);
    return *out ? HANAMI_OK : HANAMI_ERR_NOMEM;
}

static void expect_ref(git_repository *repo, const char *name, const git_oid *oid)
{
    git_reference *ref = NULL;
    assert(git_reference_lookup(&ref, repo, name) == 0);
    assert(ref && git_oid_equal(git_reference_target(ref), oid));
    git_reference_free(ref);
}

static void object(git_repository *repo, git_oid *out, const char *message)
{
    git_treebuilder *builder = NULL;
    git_tree *tree = NULL;
    git_signature *sig = NULL;
    git_oid tree_oid;
    assert(git_treebuilder_new(&builder, repo, NULL) == 0);
    assert(git_treebuilder_write(&tree_oid, builder) == 0);
    assert(git_tree_lookup(&tree, repo, &tree_oid) == 0);
    assert(git_signature_now(&sig, "fixture", "fixture@example.com") == 0);
    assert(git_commit_create_v(out, repo, NULL, sig, sig, NULL, message, tree, 0) == 0);
    git_signature_free(sig);
    git_tree_free(tree);
    git_treebuilder_free(builder);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    git_libgit2_init();
    char path[] = "/tmp/hanami-refdb-acceptance-XXXXXX";
    assert(mkdtemp(path));
    git_repository *repo = NULL;
    assert(git_repository_init(&repo, path, 0) == 0);
    git_oid old, next;
    object(repo, &old, "old");
    object(repo, &next, "next");
    const char *urls[] = {argv[1], NULL};
    char *pk = nostr_key_get_public(sk);
    assert(pk);
    hanami_signer_t signer = {.pubkey = pk, .sign = sign_event};
    hanami_nostr_ctx_t *ctx = NULL;
    assert(hanami_nostr_ctx_new(urls, &signer, &ctx) == HANAMI_OK);
    git_refdb_backend *be = NULL;
    hanami_refdb_backend_opts_t opts = {.nostr_ctx = ctx, .repo_id = "test-repo", .owner_pubkey = pk};
    assert(hanami_refdb_backend_new(&be, &opts) == HANAMI_OK);
    git_refdb *db = NULL;
    assert(git_repository_refdb(&db, repo) == 0);
    assert(git_refdb_set_backend(db, be) == 0);
    git_reference *main = NULL, *dest = NULL, *updated = NULL;
    assert(git_reference_create(&main, repo, "refs/heads/main", &old, 1, NULL) == 0);
    assert(git_reference_create(&dest, repo, "refs/heads/dest", &next, 1, NULL) == 0);
    /* The fixture rejects exactly the next write, rename and deletion. */
    assert(git_reference_set_target(&updated, main, &next, NULL) != 0);
    assert(updated == NULL);
    assert(git_error_last() && strstr(git_error_last()->message, "publish failed"));
    expect_ref(repo, "refs/heads/main", &old);
    git_reference *renamed = NULL;
    assert(git_reference_rename(&renamed, main, "refs/heads/dest", 1, NULL) != 0);
    assert(renamed == NULL);
    expect_ref(repo, "refs/heads/main", &old);
    expect_ref(repo, "refs/heads/dest", &next);
    assert(git_reference_delete(main) != 0);
    expect_ref(repo, "refs/heads/main", &old);
    assert(git_reference_set_target(&updated, main, &next, NULL) == 0);
    expect_ref(repo, "refs/heads/main", &next);
    /* Replace the backend to prove relay readback, not just cache mutation. */
    assert(hanami_refdb_backend_new(&be, &opts) == HANAMI_OK);
    assert(git_refdb_set_backend(db, be) == 0);
    expect_ref(repo, "refs/heads/main", &next);
    expect_ref(repo, "refs/heads/dest", &next);
    git_reference_free(main);
    git_reference_free(dest);
    git_reference_free(updated);
    git_refdb_free(db);
    git_repository_free(repo);
    hanami_nostr_ctx_free(ctx);
    free(pk);
    assert(hanami_test_rm_rf(path) == 0);
    git_libgit2_shutdown();
    return 0;
}
