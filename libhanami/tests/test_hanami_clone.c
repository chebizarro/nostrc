/* SPDX-License-Identifier: MIT — hanami_clone over loopback relay/Blossom/GRASP.
 *
 * Driven by clone_fixture.py: argv = blossom relay grasp tmp mode. Each mode
 * runs in a fresh process with an empty TMPDIR. */
#include <hanami/hanami.h>
#include <hanami/hanami-config.h>
#include <hanami/hanami-nostr.h>
#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr-kinds.h"
#include "keys.h"
#include <blossom/client.h>
#include <nip34.h>
#include <jansson.h>
#include <curl/curl.h>
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *sk = "0000000000000000000000000000000000000000000000000000000000000001";
static const char *blossom, *relay, *grasp, *root;
static char *pk;
static hanami_signer_t signer;

#define OK(expr) do { int rc_ = (expr); if (rc_ != 0) { \
    const git_error *e_ = git_error_last(); \
    fprintf(stderr, "%s failed: %d (%s) at %d\n", #expr, rc_, e_ ? e_->message : "", __LINE__); \
    abort(); } } while (0)

static hanami_error_t sign_event(const char *json, char **out, void *data)
{
    (void)data;
    NostrEvent *event = nostr_event_new(); assert(event);
    assert(nostr_event_deserialize_compact(event, json, NULL) == 1);
    assert(nostr_event_sign(event, sk) == 0);
    *out = nostr_event_serialize_compact(event);
    nostr_event_free(event);
    return *out ? HANAMI_OK : HANAMI_ERR_NOMEM;
}

typedef struct { char buf[64]; size_t n; } small_body;
static size_t receive(void *ptr, size_t size, size_t count, void *payload)
{
    small_body *b = payload; size_t n = size * count;
    if (b->n + n >= sizeof(b->buf)) return 0;
    memcpy(b->buf + b->n, ptr, n); b->n += n; b->buf[b->n] = 0; return n;
}
static long counter(const char *name)
{
    char url[256]; snprintf(url, sizeof(url), "%s/control/%s", blossom, name);
    CURL *curl = curl_easy_init(); assert(curl);
    small_body b = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    assert(curl_easy_perform(curl) == CURLE_OK);
    curl_easy_cleanup(curl);
    return strtol(b.buf, NULL, 10);
}

static hanami_config_t *fixture_config(const char *endpoint)
{
    hanami_config_t *config = NULL;
    const char *relays[] = { relay, NULL };
    OK(hanami_config_new(&config));
    OK(hanami_config_set_relays(config, relays, 1));
    hanami_config_set_allow_insecure(config, true);
    if (endpoint) OK(hanami_config_set_endpoint(config, endpoint));
    return config;
}

static void publish_server_list(const char *const *servers, size_t count)
{
    const char *relays[] = { relay, NULL };
    hanami_nostr_ctx_t *ctx = NULL;
    OK(hanami_nostr_ctx_new(relays, &signer, &ctx));
    NostrEvent *event = nostr_event_new(); assert(event);
    nostr_event_set_kind(event, NOSTR_KIND_BLOSSOM_SERVER_LIST);
    nostr_event_set_created_at(event, (int64_t)time(NULL));
    nostr_event_set_content(event, "");
    NostrTags *tags = nostr_tags_new(0); assert(tags);
    for (size_t i = 0; i < count; i++)
        nostr_tags_append(tags, nostr_tag_new("server", servers[i], NULL));
    nostr_event_set_tags(event, tags);
    assert(nostr_event_sign(event, sk) == 0);
    OK(hanami_nostr_publish_event(ctx, event));
    nostr_event_free(event);
    hanami_nostr_ctx_free(ctx);
}

static char *repo_config_string(git_repository *repo, const char *key)
{
    git_config *config = NULL;
    git_buf value = GIT_BUF_INIT;
    OK(git_repository_config(&config, repo));
    char *out = NULL;
    if (git_config_get_string_buf(&value, config, key) == 0) out = strdup(value.ptr);
    git_buf_dispose(&value);
    git_config_free(config);
    return out;
}

static void clone_uri(char *out, size_t n) { snprintf(out, n, "nostr://%s/repo", pk); }

/* nostrc-g2g37: a 30617 whose `clone` URL is a GRASP Git Smart HTTP endpoint
 * must never be treated as the Blossom API. */
static void test_discovery(void)
{
    const char *relays[] = { relay, NULL };
    const char *clones[] = { grasp, NULL };
    OK(hanami_announce_repo("repo", "repo", "clone fixture", clones, relays, &signer));
    char uri[160], dest[PATH_MAX];
    clone_uri(uri, sizeof(uri));
    git_repository *repo = NULL;

    /* 1. Only a Git clone URL is known: fail visibly, touch nothing. */
    hanami_config_t *config = fixture_config(NULL);
    snprintf(dest, sizeof(dest), "%s/discovery-none", root);
    assert(hanami_clone(&repo, uri, dest, NULL, config) == HANAMI_ERR_CONFIG);
    assert(!repo);
    const git_error *e = git_error_last();
    assert(e && strstr(e->message, "no Blossom server known"));
    assert(access(dest, F_OK) != 0);
    assert(counter("grasp-hits") == 0);
    hanami_config_free(config);

    /* 2. Misconfigured endpoint pointing at the Git URL: the probe
     *    classifies it as not-Blossom instead of using it. */
    config = fixture_config(grasp);
    snprintf(dest, sizeof(dest), "%s/discovery-misconfigured", root);
    assert(hanami_clone(&repo, uri, dest, NULL, config) == HANAMI_ERR_BLOSSOM);
    e = git_error_last();
    assert(e && strstr(e->message, "Blossom server"));
    assert(access(dest, F_OK) != 0);
    hanami_config_free(config);

    /* 3. Owner's BUD-03 list names the Git URL first, then the Blossom server:
     *    the probe skips the Git URL and selects the Blossom server. */
    const char *servers[] = { grasp, blossom };
    publish_server_list(servers, 2);
    config = fixture_config(NULL);
    snprintf(dest, sizeof(dest), "%s/discovery-listed", root);
    OK(hanami_clone(&repo, uri, dest, NULL, config));
    char *selected = repo_config_string(repo, "hanami.endpoint");
    assert(selected && strcmp(selected, blossom) == 0);
    free(selected);
    git_repository_free(repo);
    hanami_config_free(config);
    printf("discovery: clone URL never used as Blossom; kind-10063 server selected\n");
}


/* =========================================================================
 * Test-side publisher (independent of hanami's codec): uploads raw object
 * payloads, writes manifest segments from the documented JSON format and
 * publishes 30618 with the hanami-manifest tag. Replaced by the production
 * push path once it publishes manifests.
 * ========================================================================= */

static void path_of(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s", root, name); }

static void write_text(const char *name, const char *text)
{
    char p[PATH_MAX]; path_of(p, sizeof(p), name);
    FILE *f = fopen(p, "w"); assert(f);
    fputs(text, f); fclose(f);
}

static char *read_text(const char *name)
{
    char p[PATH_MAX]; path_of(p, sizeof(p), name);
    FILE *f = fopen(p, "r"); assert(f);
    char *buf = calloc(1, 1 << 16); assert(buf);
    size_t n = fread(buf, 1, (1 << 16) - 1, f); fclose(f);
    while (n && (buf[n - 1] == '\n')) buf[--n] = 0;
    return buf;
}

typedef struct { char oid[41]; char sha[65]; int type; size_t size; } row_t;
typedef struct { row_t *v; size_t n, cap; } rows_t;

static blossom_client_t *publisher(void)
{
    blossom_client_opts_t opts = { .endpoint = blossom, .timeout_seconds = 30,
                                   .allow_insecure = true, .user_agent = "hanami-test" };
    blossom_client_t *c = NULL;
    OK(blossom_client_new(&opts, &signer, &c));
    return c;
}

static void upload_raw(blossom_client_t *c, const void *data, size_t len, char sha[65])
{
    OK(blossom_client_address(c, data, len, sha));
    bool exists = false;
    OK(blossom_client_head(c, sha, &exists));
    if (!exists) OK(blossom_client_put(c, data, len, sha, NULL));
}

typedef struct { git_odb *odb; blossom_client_t *c; rows_t *rows; const char *skip; } upload_ctx;
static int upload_cb(const git_oid *oid, void *payload)
{
    upload_ctx *u = payload;
    char hex[41]; git_oid_tostr(hex, sizeof(hex), oid);
    if (u->skip && strstr(u->skip, hex)) return 0;
    git_odb_object *o = NULL;
    OK(git_odb_read(&o, u->odb, oid));
    if (u->rows->n == u->rows->cap) {
        u->rows->cap = u->rows->cap ? u->rows->cap * 2 : 16;
        u->rows->v = realloc(u->rows->v, u->rows->cap * sizeof(row_t)); assert(u->rows->v);
    }
    row_t *r = &u->rows->v[u->rows->n++];
    memcpy(r->oid, hex, sizeof(r->oid));
    upload_raw(u->c, git_odb_object_data(o), git_odb_object_size(o), r->sha);
    r->type = (int)git_odb_object_type(o);
    r->size = git_odb_object_size(o);
    git_odb_object_free(o);
    return 0;
}

/* Upload every object of src not listed in skip; rows describe them. */
static void upload_objects(blossom_client_t *c, git_repository *src, const char *skip, rows_t *rows)
{
    git_odb *odb = NULL;
    OK(git_repository_odb(&odb, src));
    upload_ctx u = { odb, c, rows, skip };
    OK(git_odb_foreach(odb, upload_cb, &u));
    git_odb_free(odb);
}

static char *rows_list(const rows_t *rows)
{
    char *out = calloc(rows->n + 1, 42); assert(out);
    for (size_t i = 0; i < rows->n; i++) { strcat(out, rows->v[i].oid); strcat(out, "\n"); }
    return out;
}

static int row_cmp(const void *a, const void *b) { return strcmp(((const row_t *)a)->oid, ((const row_t *)b)->oid); }

static void upload_manifest(blossom_client_t *c, const char *repo, const char *prev,
                            rows_t *rows, char head[65])
{
    qsort(rows->v, rows->n, sizeof(row_t), row_cmp);
    json_t *array = json_array();
    for (size_t i = 0; i < rows->n; i++)
        json_array_append_new(array, json_pack("{s:s,s:s,s:I,s:i}", "git_oid", rows->v[i].oid,
            "sha256", rows->v[i].sha, "size", (json_int_t)rows->v[i].size, "type", rows->v[i].type));
    json_t *doc = json_pack("{s:i,s:s,s:o,s:o}", "v", 1, "repo", repo,
                            "prev", prev ? json_string(prev) : json_null(), "rows", array);
    char *text = json_dumps(doc, JSON_COMPACT | JSON_SORT_KEYS); assert(text);
    upload_raw(c, text, strlen(text), head);
    free(text); json_decref(doc);
}

static void publish_state_refs(const char *repo, const nip34_ref_t *refs, size_t n,
                               const char *head_target, const char *manifest, int64_t at)
{
    NostrEvent *ev = nip34_create_repo_state(repo, refs, n, head_target);
    assert(ev);
    if (manifest) nostr_tags_append(ev->tags, nostr_tag_new("hanami-manifest", manifest, NULL));
    nostr_event_set_created_at(ev, at);
    assert(nostr_event_sign(ev, sk) == 0);
    const char *relays[] = { relay, NULL };
    hanami_nostr_ctx_t *ctx = NULL;
    OK(hanami_nostr_ctx_new(relays, &signer, &ctx));
    OK(hanami_nostr_publish_event(ctx, ev));
    hanami_nostr_ctx_free(ctx);
    nostr_event_free(ev);
}

static void publish_state(const char *repo, const char *main_oid, const char *manifest, int64_t at)
{
    nip34_ref_t ref = { (char *)"refs/heads/main", (char *)main_oid };
    publish_state_refs(repo, main_oid ? &ref : NULL, main_oid ? 1 : 0,
                       main_oid ? "ref: refs/heads/main" : NULL, manifest, at);
}

static void announce(const char *repo)
{
    const char *relays[] = { relay, NULL };
    const char *clones[] = { grasp, NULL };
    OK(hanami_announce_repo(repo, repo, "clone fixture", clones, relays, &signer));
}

static void make_commit(git_repository *r, const char *readme, const char *notes,
                        const git_oid *parent_oid, git_oid *out)
{
    git_oid readme_oid, empty_blob, empty_tree, notes_oid, tree_oid;
    OK(git_blob_create_from_buffer(&readme_oid, r, readme, strlen(readme)));
    OK(git_blob_create_from_buffer(&empty_blob, r, "", 0));
    git_treebuilder *tb = NULL;
    OK(git_treebuilder_new(&tb, r, NULL));
    OK(git_treebuilder_write(&empty_tree, tb));
    OK(git_treebuilder_insert(NULL, tb, "README", &readme_oid, GIT_FILEMODE_BLOB));
    OK(git_treebuilder_insert(NULL, tb, "empty", &empty_blob, GIT_FILEMODE_BLOB));
    OK(git_treebuilder_insert(NULL, tb, "emptydir", &empty_tree, GIT_FILEMODE_TREE));
    if (notes) {
        OK(git_blob_create_from_buffer(&notes_oid, r, notes, strlen(notes)));
        OK(git_treebuilder_insert(NULL, tb, "notes.txt", &notes_oid, GIT_FILEMODE_BLOB));
    }
    OK(git_treebuilder_write(&tree_oid, tb));
    git_treebuilder_free(tb);
    git_tree *tree = NULL; git_commit *parent = NULL;
    OK(git_tree_lookup(&tree, r, &tree_oid));
    if (parent_oid) OK(git_commit_lookup(&parent, r, parent_oid));
    git_signature *sig = NULL;
    OK(git_signature_new(&sig, "Hanami Test", "test@hanami.invalid", 1761600000, 0));
    const git_commit *parents[] = { parent };
    OK(git_commit_create(out, r, "refs/heads/main", sig, sig, NULL,
                         parent ? "second" : "first", tree, parent ? 1 : 0, parents));
    git_signature_free(sig); git_tree_free(tree); git_commit_free(parent);
}

static void assert_ref(git_repository *r, const char *name, const char *expect)
{
    git_oid oid; char hex[41];
    OK(git_reference_name_to_id(&oid, r, name));
    git_oid_tostr(hex, sizeof(hex), &oid);
    if (strcmp(hex, expect) != 0) { fprintf(stderr, "%s = %s, want %s\n", name, hex, expect); abort(); }
}

static void assert_objects_local(git_repository *r, const char *list)
{
    git_odb *odb = NULL;
    OK(git_repository_odb(&odb, r));
    char *dup = strdup(list), *save = NULL;
    size_t n = 0;
    for (char *tok = strtok_r(dup, "\n", &save); tok; tok = strtok_r(NULL, "\n", &save), n++) {
        git_oid oid; OK(git_oid_fromstr(&oid, tok));
        if (!git_odb_exists(odb, &oid)) { fprintf(stderr, "object %s not local\n", tok); abort(); }
    }
    assert(n > 0);
    free(dup); git_odb_free(odb);
}

static long gets_of(const char *sha) { char name[96]; snprintf(name, sizeof(name), "gets/%s", sha); return counter(name); }

/* nostrc-v2zyh: the owner published objects, a checkpoint manifest and a
 * 30618 carrying ["hanami-manifest", head]. */
static void test_seed(void)
{
    char p[PATH_MAX]; path_of(p, sizeof(p), "source");
    git_repository *src = NULL;
    OK(git_repository_init(&src, p, 0));
    OK(git_repository_set_head(src, "refs/heads/main"));
    git_oid c1; char hex[41], head[65], ts[32];
    make_commit(src, "hello hanami\n", NULL, NULL, &c1);
    blossom_client_t *c = publisher();
    rows_t rows = {0};
    upload_objects(c, src, NULL, &rows);
    assert(rows.n == 5); /* commit, root tree, README, empty blob, empty tree */
    upload_manifest(c, "repo", NULL, &rows, head);
    int64_t at = (int64_t)time(NULL);
    publish_state("repo", git_oid_tostr(hex, sizeof(hex), &c1), head, at);
    write_text("commit-1", hex); write_text("manifest-1", head);
    snprintf(ts, sizeof(ts), "%lld", (long long)at); write_text("ts-1", ts);
    char *list = rows_list(&rows); write_text("objects-1", list); free(list);
    free(rows.v); blossom_client_free(c); git_repository_free(src);
    printf("seed: 5 objects (empty blob + empty tree share one address), checkpoint %s\n", head);
}

/* Cold clone: separate process, empty TMPDIR, no prior local state. */
static void test_clone(void)
{
    char uri[160], dest[PATH_MAX], file[PATH_MAX];
    clone_uri(uri, sizeof(uri));
    path_of(dest, sizeof(dest), "clone");
    assert(access(dest, F_OK) != 0);
    hanami_config_t *config = fixture_config(NULL);
    git_repository *repo = NULL;
    OK(hanami_clone(&repo, uri, dest, NULL, config));
    git_repository_free(repo);
    hanami_config_free(config);

    /* Reopen with plain libgit2: no hanami backends, everything is local. */
    OK(git_repository_open(&repo, dest));
    char *c1 = read_text("commit-1"), *objects = read_text("objects-1"), *m1 = read_text("manifest-1");
    assert_ref(repo, "refs/remotes/origin/main", c1);
    assert_ref(repo, "HEAD", c1);
    assert_objects_local(repo, objects);
    snprintf(file, sizeof(file), "%s/README", dest);
    FILE *f = fopen(file, "r"); assert(f);
    char body[64] = {0}; assert(fread(body, 1, sizeof(body) - 1, f) > 0); fclose(f);
    assert(strcmp(body, "hello hanami\n") == 0);
    snprintf(file, sizeof(file), "%s/.git/hanami/index.sqlite", dest);
    struct stat st; assert(stat(file, &st) == 0 && st.st_size > 0);
    char *ep = repo_config_string(repo, "hanami.endpoint"), *rl = repo_config_string(repo, "hanami.relays");
    char *url = repo_config_string(repo, "remote.origin.url");
    assert(ep && !strcmp(ep, blossom) && rl && !strcmp(rl, relay) && url && !strcmp(url, uri));
    assert(gets_of(m1) == 1);
    free(ep); free(rl); free(url); free(c1); free(objects); free(m1);
    git_repository_free(repo);
    printf("clone: cold clone materialized objects, refs, checkout and cache\n");
}

/* Owner pushes a second commit: only new objects and a delta segment. */
static void test_seed2(void)
{
    char p[PATH_MAX]; path_of(p, sizeof(p), "source");
    git_repository *src = NULL;
    OK(git_repository_open(&src, p));
    char *c1hex = read_text("commit-1"), *old = read_text("objects-1"), *m1 = read_text("manifest-1");
    char *ts1 = read_text("ts-1");
    git_oid c1, c2; OK(git_oid_fromstr(&c1, c1hex));
    make_commit(src, "hello again\n", "incremental\n", &c1, &c2);
    blossom_client_t *c = publisher();
    rows_t rows = {0};
    upload_objects(c, src, old, &rows);
    assert(rows.n == 4); /* commit, root tree, README, notes.txt */
    char head[65], hex[41];
    upload_manifest(c, "repo", m1, &rows, head);
    int64_t at = (int64_t)time(NULL), prev_at = strtoll(ts1, NULL, 10);
    if (at <= prev_at) at = prev_at + 1;
    publish_state("repo", git_oid_tostr(hex, sizeof(hex), &c2), head, at);
    write_text("commit-2", hex); write_text("manifest-2", head);
    char *list = rows_list(&rows); write_text("objects-2", list); free(list);
    free(rows.v); free(c1hex); free(old); free(m1); free(ts1);
    blossom_client_free(c); git_repository_free(src);
    printf("seed2: delta segment %s -> prev checkpoint\n", head);
}

static void test_fetch(void)
{
    char dest[PATH_MAX]; path_of(dest, sizeof(dest), "clone");
    git_repository *repo = NULL;
    OK(git_repository_open(&repo, dest));
    /* Only allow_insecure: endpoint and relays come from the clone's config. */
    hanami_config_t *config = NULL;
    OK(hanami_config_new(&config));
    hanami_config_set_allow_insecure(config, true);
    OK(hanami_fetch(repo, NULL, NULL, config));
    char *c1 = read_text("commit-1"), *c2 = read_text("commit-2");
    char *m1 = read_text("manifest-1"), *m2 = read_text("manifest-2"), *objects = read_text("objects-2");
    assert_ref(repo, "refs/remotes/origin/main", c2);
    assert_ref(repo, "refs/heads/main", c1); /* local branch not moved */
    assert_objects_local(repo, objects);
    assert(gets_of(m1) == 1 && gets_of(m2) == 1); /* chain walk stopped at cached head */

    long before = counter("blob-gets");
    OK(hanami_fetch(repo, NULL, NULL, config)); /* up to date: no Blossom traffic */
    assert(counter("blob-gets") == before);
    free(c1); free(c2); free(m1); free(m2); free(objects);
    hanami_config_free(config);
    git_repository_free(repo);
    printf("fetch: incremental segment + only new objects; repeat fetch is a no-op\n");
}

static void expect_clone_failure(const char *repo_id, hanami_error_t want, const char *needle)
{
    char uri[200], dest[PATH_MAX];
    snprintf(uri, sizeof(uri), "nostr://%s/%s", pk, repo_id);
    snprintf(dest, sizeof(dest), "%s/fail-%s", root, repo_id);
    hanami_config_t *config = fixture_config(NULL);
    git_repository *repo = NULL;
    hanami_error_t got = hanami_clone(&repo, uri, dest, NULL, config);
    const git_error *e = git_error_last();
    if (got != want || !e || !strstr(e->message, needle)) {
        fprintf(stderr, "%s: got %d (%s), want %d containing '%s'\n", repo_id, got,
                e ? e->message : "", want, needle);
        abort();
    }
    assert(!repo && access(dest, F_OK) != 0);
    hanami_config_free(config);
}

static void test_negative(void)
{
    char *c1 = read_text("commit-1"), *objects = read_text("objects-1");
    char p[PATH_MAX]; path_of(p, sizeof(p), "source");
    git_repository *src = NULL;
    OK(git_repository_open(&src, p));
    blossom_client_t *c = publisher();
    int64_t at = (int64_t)time(NULL);
    char head[65];

    /* Another NIP-34 client republished 30618 without hanami's tag. */
    announce("nomanifest");
    publish_state("nomanifest", c1, NULL, at);
    expect_clone_failure("nomanifest", HANAMI_ERR_NOT_FOUND, "hanami-manifest");

    /* A manifest row maps README's OID to another object's bytes: the
     * wire hash matches the row, but the Git OID re-hash must reject it. */
    rows_t rows = {0};
    upload_objects(c, src, NULL, &rows);
    const char *empty_sha = NULL;
    for (size_t i = 0; i < rows.n; i++) if (rows.v[i].size == 0) empty_sha = rows.v[i].sha;
    assert(empty_sha);
    for (size_t i = 0; i < rows.n; i++)
        if (rows.v[i].type == GIT_OBJECT_BLOB && rows.v[i].size > 0) {
            memcpy(rows.v[i].sha, empty_sha, 65); rows.v[i].size = 0;
        }
    announce("tamper");
    upload_manifest(c, "tamper", NULL, &rows, head);
    publish_state("tamper", c1, head, at);
    expect_clone_failure("tamper", HANAMI_ERR_BLOSSOM, "does not hash to Git OID");

    /* Coverage: the 30618 tip is not in the manifest. */
    rows_t partial = {0};
    for (size_t i = 0; i < rows.n; i++)
        if (rows.v[i].type != GIT_OBJECT_COMMIT) {
            if (partial.n == partial.cap) { partial.cap = partial.cap ? partial.cap * 2 : 8;
                partial.v = realloc(partial.v, partial.cap * sizeof(row_t)); }
            partial.v[partial.n++] = rows.v[i];
        }
    announce("partial");
    upload_manifest(c, "partial", NULL, &partial, head);
    publish_state("partial", c1, head, at);
    expect_clone_failure("partial", HANAMI_ERR_INDEX, "absent from manifest");

    /* Strict decode: another repository's manifest spliced into this 30618,
     * and a segment whose rows are not strictly ascending by OID. */
    char *m1 = read_text("manifest-1");
    announce("splice");
    publish_state("splice", c1, m1, at);
    expect_clone_failure("splice", HANAMI_ERR_INDEX, "manifest chain");
    free(m1);
    rows_t good = {0};
    upload_objects(c, src, NULL, &good);
    qsort(good.v, good.n, sizeof(row_t), row_cmp);
    json_t *array = json_array();
    for (size_t i = good.n; i-- > 0; )
        json_array_append_new(array, json_pack("{s:s,s:s,s:I,s:i}", "git_oid", good.v[i].oid,
            "sha256", good.v[i].sha, "size", (json_int_t)good.v[i].size, "type", good.v[i].type));
    json_t *doc = json_pack("{s:i,s:s,s:n,s:o}", "v", 1, "repo", "unsorted", "prev", "rows", array);
    char *text = json_dumps(doc, JSON_COMPACT); assert(text);
    upload_raw(c, text, strlen(text), head);
    free(text); json_decref(doc); free(good.v);
    announce("unsorted");
    publish_state("unsorted", c1, head, at);
    expect_clone_failure("unsorted", HANAMI_ERR_INDEX, "manifest chain");

    /* Chained symbolic refs: HEAD -> default -> main resolves (bounded);
     * a cycle fails visibly instead of silently dropping HEAD. */
    rows_t all = {0};
    upload_objects(c, src, NULL, &all);
    char chain_head[65];
    upload_manifest(c, "chain", NULL, &all, chain_head);
    nip34_ref_t chain_refs[] = {
        { (char *)"refs/heads/main", c1 },
        { (char *)"refs/heads/default", (char *)"ref: refs/heads/main" },
    };
    announce("chain");
    publish_state_refs("chain", chain_refs, 2, "ref: refs/heads/default", chain_head, at);
    {
        char uri[200], dest[PATH_MAX];
        snprintf(uri, sizeof(uri), "nostr://%s/chain", pk);
        path_of(dest, sizeof(dest), "chain-clone");
        hanami_config_t *config = fixture_config(NULL);
        git_repository *repo = NULL;
        OK(hanami_clone(&repo, uri, dest, NULL, config));
        assert_ref(repo, "HEAD", c1);
        git_repository_free(repo);
        hanami_config_free(config);
    }
    upload_manifest(c, "cycle", NULL, &all, chain_head);
    nip34_ref_t cycle_refs[] = {
        { (char *)"refs/heads/main", c1 },
        { (char *)"refs/heads/a", (char *)"ref: refs/heads/b" },
        { (char *)"refs/heads/b", (char *)"ref: refs/heads/a" },
    };
    announce("cycle");
    publish_state_refs("cycle", cycle_refs, 3, "ref: refs/heads/a", chain_head, at);
    expect_clone_failure("cycle", HANAMI_ERR_NOSTR, "cyclic");
    free(all.v);

    /* Failure cleanup never removes what the clone did not create: an
     * existing empty directory survives (emptied of clone output), a
     * non-empty one is refused untouched. */
    {
        char uri[200], dest[PATH_MAX], sentinel[PATH_MAX];
        snprintf(uri, sizeof(uri), "nostr://%s/nomanifest", pk);
        path_of(dest, sizeof(dest), "preexisting-empty");
        assert(mkdir(dest, 0755) == 0);
        hanami_config_t *config = fixture_config(NULL);
        git_repository *repo = NULL;
        assert(hanami_clone(&repo, uri, dest, NULL, config) == HANAMI_ERR_NOT_FOUND);
        struct stat st;
        assert(stat(dest, &st) == 0 && S_ISDIR(st.st_mode));
        char gitdir[PATH_MAX + 8]; snprintf(gitdir, sizeof(gitdir), "%s/.git", dest);
        assert(access(gitdir, F_OK) != 0);
        path_of(dest, sizeof(dest), "preexisting-full");
        assert(mkdir(dest, 0755) == 0);
        snprintf(sentinel, sizeof(sentinel), "%s/keep.txt", dest);
        FILE *f = fopen(sentinel, "w"); assert(f); fputs("mine", f); fclose(f);
        assert(hanami_clone(&repo, uri, dest, NULL, config) == HANAMI_ERR_INVALID_ARG);
        assert(access(sentinel, F_OK) == 0);
        hanami_config_free(config);
    }

    /* A chain deeper than the reader's hard limit (64) is refused. */
    rows_t none = {0};
    char prev[65] = {0};
    for (int i = 0; i < 65; i++) {
        upload_manifest(c, "deep", i ? prev : NULL, &none, head);
        memcpy(prev, head, 65);
    }
    announce("deep");
    publish_state("deep", NULL, head, at);
    expect_clone_failure("deep", HANAMI_ERR_INDEX, "manifest chain");

    free(rows.v); free(partial.v); free(c1); free(objects);
    blossom_client_free(c); git_repository_free(src);
    printf("negative: missing tag, tampered row, uncovered tip, spliced/unsorted segment, "
           "cyclic symref, over-deep chain all fail visibly; chained HEAD resolves; "
           "cleanup keeps pre-existing directories\n");
}

int main(int argc, char **argv)
{
    if (argc != 6) { fprintf(stderr, "usage: %s blossom relay grasp tmp mode\n", argv[0]); return 2; }
    blossom = argv[1]; relay = argv[2]; grasp = argv[3]; root = argv[4];
    const char *mode = argv[5];
    git_libgit2_init();
    assert(hanami_init() == HANAMI_OK);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    pk = nostr_key_get_public(sk); assert(pk);
    signer = (hanami_signer_t){ .pubkey = pk, .sign = sign_event };

    if (!strcmp(mode, "discovery")) test_discovery();
    else if (!strcmp(mode, "seed")) test_seed();
    else if (!strcmp(mode, "clone")) test_clone();
    else if (!strcmp(mode, "seed2")) test_seed2();
    else if (!strcmp(mode, "fetch")) test_fetch();
    else if (!strcmp(mode, "negative")) test_negative();
    else { fprintf(stderr, "unknown mode %s\n", mode); return 2; }

    free(pk);
    curl_global_cleanup();
    hanami_shutdown();
    git_libgit2_shutdown();
    return 0;
}
