/* SPDX-License-Identifier: MIT — production entrypoints over loopback wire services. */
#include <hanami/hanami.h>
#include <hanami/hanami-config.h>
#include <hanami/hanami-grasp.h>
#include <porthome/head.h>
#include <porthome/dir-node.h>
#include <blossom/client.h>
#include <hanami/hanami-nostr.h>
#include "nostr-event.h"
#include "keys.h"
#include "hanami_test_fs.h"
#include <curl/curl.h>
#include <jansson.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *sk = "0000000000000000000000000000000000000000000000000000000000000001";
static const char *id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char content[] = "only the vault can read this\n";

#define OK(expr) do { int rc_ = (expr); if (rc_ != 0) { \
    const git_error *e_ = git_error_last(); \
    fprintf(stderr, "%s failed: %d (%s) at %d\n", #expr, rc_, e_ ? e_->message : "", __LINE__); \
    abort(); } } while (0)

static hanami_error_t sign_event(const char *json, char **out, void *data)
{
    const char *signing_key = data ? data : sk;
    NostrEvent *event = nostr_event_new(); assert(event);
    assert(nostr_event_deserialize_compact(event, json, NULL) == 1);
    assert(nostr_event_sign(event, signing_key) == 0);
    *out = nostr_event_serialize_compact(event);
    nostr_event_free(event);
    return *out ? HANAMI_OK : HANAMI_ERR_NOMEM;
}

typedef struct { uint8_t *p; size_t n; } response;
static size_t receive(void *ptr, size_t size, size_t count, void *payload)
{
    response *r = payload; size_t n = size * count;
    uint8_t *p = realloc(r->p, r->n + n + 1);
    if (!p) return 0;
    r->p = p; memcpy(p + r->n, ptr, n); r->n += n; p[r->n] = 0; return n;
}
static response get(const char *endpoint, const char *path)
{
    char url[512]; snprintf(url, sizeof(url), "%s/%s", endpoint, path);
    CURL *curl = curl_easy_init(); assert(curl);
    response r = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    assert(curl_easy_perform(curl) == CURLE_OK);
    long status; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    assert(status == 200); curl_easy_cleanup(curl); return r;
}
static void control(const char *endpoint, const char *mode)
{
    char path[128]; snprintf(path, sizeof(path), "control/%s", mode);
    response r = get(endpoint, path); free(r.p);
}

static void free_temp_repo(git_repository *repo)
{
    char *path = strdup(git_repository_path(repo)); assert(path);
    git_repository_free(repo);
    assert(hanami_test_rm_rf(path) == 0); free(path);
}

static void verify_opaque_objects(const char *endpoint, const char *pk, const porthome_ring_t *ring)
{
    response r = get(endpoint, "head");
    NostrEvent *event = nostr_event_new();
    assert(nostr_event_deserialize_compact(event, (char *)r.p, NULL) == 1); free(r.p);
    uint8_t vault_id[32]; assert(porthome_from_hex64(id, vault_id) == 0);
    assert(strcmp(nostr_event_get_pubkey(event), pk) == 0);
    porthome_head *head = NULL;
    assert(porthome_head_event_open(event, vault_id, ring, 1, &head) == 0);
    assert(head->has_ref_state && head->seq > 0);
    char address[65]; porthome_hex64(head->ref_state, address);
    r = get(endpoint, address);
    uint8_t sha[32]; assert(porthome_sha256(r.p, r.n, sha) == 0);
    assert(memcmp(sha, head->ref_state, 32) == 0);
    uint8_t *plain = NULL; size_t len = 0;
    assert(porthome_decrypt_manifest_v2(ring, 1, r.p, r.n, &plain, &len) == 0); free(r.p);
    json_t *state = json_loadb((char *)plain, len, JSON_REJECT_DUPLICATES, NULL); assert(state); free(plain);
    json_t *rows = json_object_get(state, "index"); assert(json_array_size(rows) >= 3);
    porthome_ring_entry_t wrong_key = {.epoch = 1, .node_key = {0xff}};
    porthome_ring_t wrong = {&wrong_key, 1}, absent = {NULL, 0};
    size_t i; json_t *row;
    json_array_foreach(rows, i, row) {
        const char *hash = json_string_value(json_object_get(row, "sealed_sha256"));
        size_t size = (size_t)json_integer_value(json_object_get(row, "size"));
        uint64_t epoch = (uint64_t)json_integer_value(json_object_get(row, "chunk_key_id"));
        assert(epoch == 1); r = get(endpoint, hash);
        assert(r.p[0] == PORTHOME_WIRE_VERSION_V2);
        assert(porthome_decrypt_chunk(wrong_key.node_key, r.p, r.n, &plain, &len) != 0);
        assert(porthome_decrypt_chunk_v2(&wrong, epoch, r.p, r.n, size, &plain, &len) != 0);
        assert(porthome_decrypt_chunk_v2(&absent, epoch, r.p, r.n, size, &plain, &len) == PORTHOME_ERR_EPOCH);
        assert(porthome_decrypt_chunk_v2(ring, epoch, r.p, r.n, size, &plain, &len) == 0);
        assert(len == size); free(plain); free(r.p);
    }
    json_decref(state); porthome_head_dispose(head); free(head); nostr_event_free(event);
}

static void commit_file(git_repository *repo, const char *branch, const char *name,
    const char *bytes, const git_oid *parent_id, git_oid *out)
{
    git_commit *parent = NULL; git_tree *base = NULL, *tree = NULL;
    git_treebuilder *builder = NULL; git_signature *sig = NULL;
    git_oid blob, tree_id;
    if (parent_id) { OK(git_commit_lookup(&parent, repo, parent_id)); OK(git_commit_tree(&base, parent)); }
    OK(git_blob_create_frombuffer(&blob, repo, bytes, strlen(bytes)));
    OK(git_treebuilder_new(&builder, repo, base));
    OK(git_treebuilder_insert(NULL, builder, name, &blob, GIT_FILEMODE_BLOB));
    OK(git_treebuilder_write(&tree_id, builder)); OK(git_tree_lookup(&tree, repo, &tree_id));
    OK(git_signature_new(&sig, "Writer", "writer@example.test", 1234567900, 0));
    OK(git_commit_create(out, repo, branch, sig, sig, NULL, "writer branch", tree,
                         parent ? 1 : 0, parent ? (const git_commit **)&parent : NULL));
    git_signature_free(sig); git_tree_free(tree); git_tree_free(base);
    git_treebuilder_free(builder); git_commit_free(parent);
}

/* Membership policy is an OWNER-signed root, assembled using public porthome
 * primitives. This test does not fake membership in a writer-supplied head. */
static void authorize_writer(const char *endpoint, const char **relays,
    const char *pk, const char *member, const porthome_ring_t *ring, const hanami_signer_t *signer)
{
    response r = get(endpoint, "head");
    NostrEvent *old = nostr_event_new(); assert(nostr_event_deserialize_compact(old, (char *)r.p, NULL) == 1);
    free(r.p); uint8_t vault_id[32]; porthome_from_hex64(id, vault_id);
    porthome_head *head = NULL; assert(porthome_head_event_open(old, vault_id, ring, 1, &head) == 0);
    char hash[65]; porthome_hex64(head->root_manifest, hash); r = get(endpoint, hash);
    porthome_dir_node *root = NULL; assert(porthome_dir_node_open(ring, 1, r.p, r.n, &root) == 0); free(r.p);
    root->vault->writers = realloc(root->vault->writers, 2 * sizeof(*root->vault->writers)); assert(root->vault->writers);
    porthome_from_hex64(member, root->vault->writers[1]); root->vault->writers_len = 2; root->vault->grants_rev++;
    porthome_head_parent parent = {.seq = head->seq};
    porthome_from_hex64(pk, parent.writer); memcpy(parent.root_manifest, head->root_manifest, 32);
    uint8_t *sealed = NULL; size_t len = 0;
    assert(porthome_dir_node_seal(root, ring->entries[0].node_key, &sealed, &len, NULL) == 0);
    blossom_client_opts_t opts = {.endpoint = endpoint, .allow_insecure = true};
    blossom_client_t *client = NULL; OK(blossom_client_new(&opts, signer, &client));
    OK(blossom_client_address(client, sealed, len, hash)); OK(blossom_client_put(client, sealed, len, hash, NULL));
    porthome_from_hex64(hash, head->root_manifest); free(sealed); blossom_client_free(client);
    free(head->parents); head->parents = malloc(sizeof(parent)); assert(head->parents);
    *head->parents = parent; head->parents_len = 1;
    head->grants_rev = root->vault->grants_rev; head->seq++; head->written_at++;
    NostrEvent *event = NULL; assert(porthome_head_event_new(head, ring->entries[0].node_key, parent.writer, &event) == 0);
    assert(nostr_event_sign(event, sk) == 0);
    hanami_nostr_ctx_t *ctx = NULL; OK(hanami_nostr_ctx_new((const char *const *)relays, signer, &ctx));
    OK(hanami_nostr_publish_event(ctx, event)); hanami_nostr_ctx_free(ctx);
    nostr_event_free(event); nostr_event_free(old); porthome_head_dispose(head); free(head);
    porthome_dir_node_dispose(root); free(root);
}

int main(int argc, char **argv)
{
    assert(argc == 5);
    const char *endpoint = argv[1], *relays[] = {argv[2], NULL};
    char *pk = nostr_key_get_public(sk); assert(pk);
    hanami_signer_t signer = {.pubkey = pk, .sign = sign_event};
    porthome_ring_entry_t key = {.epoch = 1, .node_key = {0x42}};
    porthome_ring_t ring = {&key, 1};
    hanami_config_t *config = NULL; OK(hanami_config_new(&config));
    assert(!hanami_config_get_allow_insecure(config));
    hanami_config_set_allow_insecure(config, true);
    git_libgit2_init(); OK(hanami_init());
    git_repository *repo = NULL;
    OK(hanami_repo_open(&repo, endpoint, relays, id, pk, &signer, config, &ring));
    if (!strcmp(argv[4], "private")) {
        git_oid blob, tree_oid, commit;
        OK(git_blob_create_frombuffer(&blob, repo, content, sizeof(content) - 1));
        git_treebuilder *builder = NULL; OK(git_treebuilder_new(&builder, repo, NULL));
        OK(git_treebuilder_insert(NULL, builder, "secret.txt", &blob, GIT_FILEMODE_BLOB));
        OK(git_treebuilder_write(&tree_oid, builder)); git_treebuilder_free(builder);
        git_tree *tree = NULL; OK(git_tree_lookup(&tree, repo, &tree_oid));
        git_signature *sig = NULL; OK(git_signature_new(&sig, "Fixture", "fixture@example.test", 1234567890, 0));
        OK(git_commit_create(&commit, repo, "refs/heads/main", sig, sig, NULL, "sealed commit", tree, 0, NULL));
        git_tree_free(tree); git_signature_free(sig);
        git_reference *ref = NULL;
        control(endpoint, "reject");
        assert(git_reference_create(&ref, repo, "refs/heads/rejected", &commit, 0, NULL) < 0);
        assert(git_reference_lookup(&ref, repo, "refs/heads/rejected") == GIT_ENOTFOUND);
        OK(git_reference_lookup(&ref, repo, "refs/heads/main"));
        git_reference *renamed = NULL;
        assert(git_reference_rename(&renamed, ref, "refs/heads/renamed", 0, NULL) < 0);
        assert(git_reference_delete(ref) < 0); git_reference_free(ref);
        OK(git_reference_lookup(&ref, repo, "refs/heads/main")); git_reference_free(ref);
        assert(git_reference_lookup(&ref, repo, "refs/heads/renamed") == GIT_ENOTFOUND);
        control(endpoint, "accept");
        hanami_grasp_push_opts_t grasp = {
            .clone_url = "https://example.test/npub1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqspc55m/vault.git",
            .signer = &signer, .repo_id = id,
            .relay_urls = (const char *const *)relays, .relay_count = 1
        };
        assert(hanami_push_to_grasp(repo, &grasp) == HANAMI_ERR_AUTH);
        assert(hanami_push_to_blossom(repo, endpoint, &signer, relays, id) == HANAMI_ERR_AUTH);
        verify_opaque_objects(endpoint, pk, &ring);
        free_temp_repo(repo); repo = NULL;
        char path[1024]; snprintf(path, sizeof(path), "%s/clone", argv[3]);
        OK(hanami_clone_sealed(&repo, path, endpoint, relays, id, pk, NULL, &ring, config));
        git_odb *odb = NULL; git_odb_object *object = NULL; OK(git_repository_odb(&odb, repo));
        OK(git_odb_read(&object, odb, &blob));
        assert(git_odb_object_size(object) == sizeof(content) - 1);
        assert(memcmp(git_odb_object_data(object), content, sizeof(content) - 1) == 0);
        git_odb_object_free(object); git_odb_free(odb);
        char file[1100]; snprintf(file, sizeof(file), "%s/secret.txt", path);
        FILE *f = fopen(file, "rb"); assert(f); char got[128] = {0};
        assert(fread(got, 1, sizeof(got), f) == sizeof(content) - 1); fclose(f);
        assert(!strcmp(got, content));
        git_repository_free(repo); repo = NULL;
        OK(git_repository_open(&repo, path));
        assert(hanami_push_to_grasp(repo, &grasp) == HANAMI_ERR_AUTH);
        assert(hanami_push_to_blossom(repo, endpoint, &signer, relays, id) == HANAMI_ERR_AUTH);
        git_repository_free(repo); repo = NULL;
        porthome_ring_entry_t bad = {.epoch = 1, .node_key = {0xff}};
        porthome_ring_t wrong = {&bad, 1};
        assert(hanami_repo_open(&repo, endpoint, relays, id, pk, NULL, config, &wrong) != HANAMI_OK);
        assert(!repo);
        bad = key; bad.epoch = 2;
        assert(hanami_repo_open(&repo, endpoint, relays, id, pk, NULL, config, &wrong) != HANAMI_OK);
        assert(!repo);
        control(endpoint, "corrupt");
        assert(hanami_repo_open(&repo, endpoint, relays, id, pk, NULL, config, &ring) != HANAMI_OK);
        assert(!repo); control(endpoint, "restore");

        const char *member_sk = "0000000000000000000000000000000000000000000000000000000000000002";
        char *member_pk = nostr_key_get_public(member_sk); assert(member_pk);
        hanami_signer_t member_signer = {.pubkey = member_pk, .sign = sign_event, .user_data = (void *)member_sk};
        authorize_writer(endpoint, relays, pk, member_pk, &ring, &signer);
        git_repository *member_repo = NULL;
        OK(hanami_repo_open(&member_repo, endpoint, relays, id, pk, &member_signer, config, &ring));
        git_oid feature, owner_tip;
        commit_file(member_repo, "refs/heads/feature", "secret.txt", "member's independent edit\n", &commit, &feature);
        free_temp_repo(member_repo);
        OK(hanami_repo_open(&repo, endpoint, relays, id, pk, &signer, config, &ring));
        commit_file(repo, "refs/heads/main", "owner.txt", "owner's concurrent edit\n", &commit, &owner_tip);
        OK(hanami_repo_fetch_writer(repo, member_pk));
        char remote_ref[256]; snprintf(remote_ref, sizeof(remote_ref), "refs/remotes/%s/feature", member_pk);
        OK(git_reference_lookup(&ref, repo, remote_ref));
        assert(git_oid_equal(git_reference_target(ref), &feature)); git_reference_free(ref);
        OK(git_reference_lookup(&ref, repo, "refs/heads/main"));
        assert(git_oid_equal(git_reference_target(ref), &owner_tip)); git_reference_free(ref);
        git_commit *ours = NULL, *theirs = NULL;
        OK(git_commit_lookup(&ours, repo, &owner_tip)); OK(git_commit_lookup(&theirs, repo, &feature));
        git_index *merged = NULL; OK(git_merge_commits(&merged, repo, ours, theirs, NULL));
        assert(!git_index_has_conflicts(merged));
        git_oid merged_tree_id, merged_id; OK(git_index_write_tree_to(&merged_tree_id, merged, repo));
        git_tree *merged_tree = NULL; OK(git_tree_lookup(&merged_tree, repo, &merged_tree_id));
        const git_commit *parents[] = {ours, theirs};
        OK(git_signature_new(&sig, "Owner", "owner@example.test", 1234568000, 0));
        OK(git_commit_create(&merged_id, repo, "refs/heads/main", sig, sig, NULL,
                             "explicit Git merge, not vault fold", merged_tree, 2, parents));
        assert(git_graph_descendant_of(repo, &merged_id, &feature) == 1);
        assert(git_graph_descendant_of(repo, &merged_id, &owner_tip) == 1);
        git_signature_free(sig); git_tree_free(merged_tree); git_index_free(merged);
        git_commit_free(ours); git_commit_free(theirs);
        free_temp_repo(repo); repo = NULL; free(member_pk);
    } else {
        OK(hanami_repo_lift_privacy(repo));
        free_temp_repo(repo); repo = NULL;
        OK(hanami_repo_open(&repo, endpoint, relays, id, pk, &signer, config, &ring));
        hanami_grasp_push_opts_t grasp = {0};
        assert(hanami_push_to_grasp(repo, &grasp) == HANAMI_ERR_INVALID_ARG);
        OK(hanami_push_to_blossom(repo, endpoint, &signer, relays, id));
        free_temp_repo(repo); repo = NULL;
        /* The member's old private branch cannot undo the owner's lift. */
        const char *member_sk = "0000000000000000000000000000000000000000000000000000000000000002";
        char *member_pk = nostr_key_get_public(member_sk); assert(member_pk);
        hanami_signer_t member = {.pubkey = member_pk, .sign = sign_event, .user_data = (void *)member_sk};
        OK(hanami_repo_open(&repo, endpoint, relays, id, pk, &member, config, &ring));
        OK(hanami_push_to_blossom(repo, endpoint, &member, relays, id));
        free_temp_repo(repo); free(member_pk);
    }
    hanami_shutdown(); git_libgit2_shutdown(); hanami_config_free(config); free(pk);
    puts("test_hanami_sealed_roundtrip: OK"); return 0;
}
