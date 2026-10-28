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
#include <sqlite3.h>
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

/* data: secret key, or NULL for sk. */
static hanami_error_t sign_event(const char *json, char **out, void *data)
{
    NostrEvent *event = nostr_event_new(); assert(event);
    assert(nostr_event_deserialize_compact(event, json, NULL) == 1);
    assert(nostr_event_sign(event, data ? (const char *)data : sk) == 0);
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

static hanami_config_t *fixture_config_on(const char *relay_url, const char *endpoint)
{
    hanami_config_t *config = NULL;
    const char *relays[] = { relay_url, NULL };
    OK(hanami_config_new(&config));
    OK(hanami_config_set_relays(config, relays, 1));
    hanami_config_set_allow_insecure(config, true);
    if (endpoint) OK(hanami_config_set_endpoint(config, endpoint));
    return config;
}

static hanami_config_t *fixture_config(const char *endpoint)
{
    return fixture_config_on(relay, endpoint);
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

static int row_cmp(const void *a, const void *b) { return strcmp(((const row_t *)a)->oid, ((const row_t *)b)->oid); }

static void upload_manifest(blossom_client_t *c, const char *repo, const char *prev,
                            rows_t *rows, char head[65])
{
    if (rows->n) qsort(rows->v, rows->n, sizeof(row_t), row_cmp);
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


/* ---- production push helpers ---- */

typedef struct { char *p; size_t n; } dyn_body;
static size_t receive_dyn(void *ptr, size_t size, size_t count, void *payload)
{
    dyn_body *b = payload; size_t n = size * count;
    char *grown = realloc(b->p, b->n + n + 1);
    if (!grown) return 0;
    b->p = grown; memcpy(b->p + b->n, ptr, n); b->n += n; b->p[b->n] = 0; return n;
}
static char *control_get(const char *path)
{
    char url[512]; snprintf(url, sizeof(url), "%s/control/%s", blossom, path);
    CURL *curl = curl_easy_init(); assert(curl);
    dyn_body b = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_dyn);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    assert(curl_easy_perform(curl) == CURLE_OK);
    long status = 0; curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    assert(status == 200);
    curl_easy_cleanup(curl);
    return b.p ? b.p : strdup("");
}
static void control_set(const char *flag, bool on)
{
    char path[96]; snprintf(path, sizeof(path), "set/%s/%s", flag, on ? "on" : "off");
    free(control_get(path));
}

/* Segment as stored on Blossom, read without touching the GET counters. */
static json_t *peek_segment(const char *sha)
{
    char path[96]; snprintf(path, sizeof(path), "blob/%s", sha);
    char *body = control_get(path);
    json_t *doc = json_loads(body, 0, NULL); assert(doc);
    free(body);
    return doc;
}

static char *cache_meta(const char *repo_dir, const char *key)
{
    char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/%s/.git/hanami/index.sqlite", root, repo_dir);
    sqlite3 *db = NULL; sqlite3_stmt *st = NULL;
    assert(sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    assert(sqlite3_prepare_v2(db, "SELECT value FROM hanami_meta WHERE key = ?", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
    char *out = sqlite3_step(st) == SQLITE_ROW ? strdup((const char *)sqlite3_column_text(st, 0)) : NULL;
    sqlite3_finalize(st); sqlite3_close(db);
    return out;
}

static hanami_error_t push_source(git_repository *src)
{
    const char *relays[] = { relay, NULL };
    hanami_config_t *config = fixture_config(NULL);
    hanami_error_t err = hanami_push_to_blossom_ex(src, blossom, &signer, relays, "repo", config);
    hanami_config_free(config);
    return err;
}

static void published_main(char out[41])
{
    const char *relays[] = { relay, NULL };
    hanami_nostr_ctx_t *ctx = NULL;
    OK(hanami_nostr_ctx_new(relays, NULL, &ctx));
    nip34_repo_state_t *state = NULL;
    OK(hanami_nostr_fetch_state(ctx, "repo", pk, &state));
    out[0] = 0;
    for (size_t i = 0; i < state->ref_count; i++)
        if (!strcmp(state->refs[i].refname, "refs/heads/main"))
            snprintf(out, 41, "%s", state->refs[i].target);
    nip34_repo_state_free(state);
    hanami_nostr_ctx_free(ctx);
}

static int list_cb(const git_oid *oid, void *payload)
{
    char hex[41]; git_oid_tostr(hex, sizeof(hex), oid);
    strcat((char *)payload, hex); strcat((char *)payload, "\n");
    return 0;
}

/* Newline-separated OIDs of every object in r's ODB. */
static char *odb_list(git_repository *r)
{
    git_odb *odb = NULL;
    OK(git_repository_odb(&odb, r));
    char *out = calloc(1, 1 << 20); assert(out);
    OK(git_odb_foreach(odb, list_cb, out));
    git_odb_free(odb);
    return out;
}

/* nostrc-v2zyh: the owner pushes with the production path: objects, then a
 * checkpoint manifest, then a 30618 carrying ["hanami-manifest", head]. */
static void test_seed(void)
{
    char p[PATH_MAX]; path_of(p, sizeof(p), "source");
    git_repository *src = NULL;
    OK(git_repository_init(&src, p, 0));
    OK(git_repository_set_head(src, "refs/heads/main"));
    git_oid c1; char hex[41], main_now[41];
    make_commit(src, "hello hanami\n", NULL, NULL, &c1);
    OK(push_source(src));
    git_oid_tostr(hex, sizeof(hex), &c1);
    published_main(main_now);
    assert(!strcmp(main_now, hex));
    char *head = cache_meta("source", "manifest_head"), *depth = cache_meta("source", "manifest_depth");
    assert(head && depth && !strcmp(depth, "1"));
    json_t *seg = peek_segment(head);
    assert(json_is_null(json_object_get(seg, "prev")));
    assert(json_array_size(json_object_get(seg, "rows")) == 5); /* incl. empty blob + empty tree */
    json_decref(seg);
    char *list = odb_list(src);
    write_text("commit-1", hex); write_text("manifest-1", head); write_text("objects-1", list);
    free(list); free(head); free(depth);
    git_repository_free(src);
    printf("seed: hanami_push_to_blossom published 5 objects, checkpoint, tagged 30618\n");
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
    char *c1hex = read_text("commit-1"), *m1 = read_text("manifest-1"), *old = read_text("objects-1");
    git_oid c1, c2; OK(git_oid_fromstr(&c1, c1hex));
    make_commit(src, "hello again\n", "incremental\n", &c1, &c2);
    OK(push_source(src));
    char hex[41], main_now[41];
    git_oid_tostr(hex, sizeof(hex), &c2);
    published_main(main_now);
    assert(!strcmp(main_now, hex));
    char *head = cache_meta("source", "manifest_head"), *depth = cache_meta("source", "manifest_depth");
    assert(head && strcmp(head, m1) && depth && !strcmp(depth, "2"));
    json_t *seg = peek_segment(head);
    assert(!strcmp(json_string_value(json_object_get(seg, "prev")), m1));
    assert(json_array_size(json_object_get(seg, "rows")) == 4); /* delta: commit, tree, README, notes */
    json_decref(seg);
    char *all = odb_list(src), news[1 << 14] = {0};
    char *save = NULL;
    for (char *tok = strtok_r(all, "\n", &save); tok; tok = strtok_r(NULL, "\n", &save))
        if (!strstr(old, tok)) { strcat(news, tok); strcat(news, "\n"); }
    write_text("commit-2", hex); write_text("manifest-2", head); write_text("objects-2", news);
    free(all); free(head); free(depth); free(c1hex); free(m1); free(old);
    git_repository_free(src);
    printf("seed2: incremental push published a 4-row delta segment\n");
}

/* A push reports success only if the manifest upload and the 30618 OK both
 * succeed; failed pushes leave rows unpublished and are completed later. */
static void test_pushfail(void)
{
    char p[PATH_MAX]; path_of(p, sizeof(p), "source");
    git_repository *src = NULL;
    OK(git_repository_open(&src, p));
    char *c2hex = read_text("commit-2"), *m2 = read_text("manifest-2");
    git_oid c2, c3; OK(git_oid_fromstr(&c2, c2hex));
    make_commit(src, "third\n", "third notes\n", &c2, &c3);
    char hex[41], main_now[41];
    git_oid_tostr(hex, sizeof(hex), &c3);

    control_set("reject-manifest", true);
    hanami_error_t err = push_source(src);
    control_set("reject-manifest", false);
    assert(err != HANAMI_OK);
    published_main(main_now); assert(!strcmp(main_now, c2hex));

    control_set("relay-reject", true);
    err = push_source(src);
    control_set("relay-reject", false);
    assert(err != HANAMI_OK);
    const git_error *e = git_error_last();
    assert(e && strstr(e->message, "not accepted"));
    published_main(main_now); assert(!strcmp(main_now, c2hex));
    char *head = cache_meta("source", "manifest_head");
    assert(!strcmp(head, m2)); /* failed pushes never advance the cached head */
    free(head);

    OK(push_source(src));
    published_main(main_now); assert(!strcmp(main_now, hex));
    head = cache_meta("source", "manifest_head");
    json_t *seg = peek_segment(head);
    assert(!strcmp(json_string_value(json_object_get(seg, "prev")), m2));
    assert(json_array_size(json_object_get(seg, "rows")) == 4); /* commit, tree, README, notes */
    json_decref(seg);
    write_text("commit-3", hex);
    free(head); free(c2hex); free(m2);
    git_repository_free(src);
    printf("pushfail: manifest-upload and relay rejections fail the push; retry publishes\n");
}

/* Chain bound: once 16 segments are linked, the next push is a checkpoint
 * holding every row; fetch and a fresh cold clone read only that segment. */
static void test_checkpoint(void)
{
    char p[PATH_MAX]; path_of(p, sizeof(p), "source");
    git_repository *src = NULL;
    OK(git_repository_open(&src, p));
    char *c3hex = read_text("commit-3");
    git_oid parent, next; OK(git_oid_fromstr(&parent, c3hex)); free(c3hex);
    char *depth = cache_meta("source", "manifest_depth");
    long prev_depth = strtol(depth, NULL, 10); free(depth);
    bool reset = false;
    for (int i = 0; i < 20 && !reset; i++) {
        char notes[32]; snprintf(notes, sizeof(notes), "push %d\n", i);
        make_commit(src, "checkpoint run\n", notes, &parent, &next);
        parent = next;
        OK(push_source(src));
        depth = cache_meta("source", "manifest_depth");
        long d = strtol(depth, NULL, 10); free(depth);
        if (d == 1) { assert(prev_depth == 16); reset = true; }
        else assert(d == prev_depth + 1 && d <= 16);
        prev_depth = d;
    }
    assert(reset);
    char *head = cache_meta("source", "manifest_head");
    json_t *seg = peek_segment(head);
    char *all = odb_list(src);
    size_t objects = 0;
    for (char *q = all; *q; q++) objects += *q == '\n';
    assert(json_is_null(json_object_get(seg, "prev")));
    assert(json_array_size(json_object_get(seg, "rows")) == objects);
    json_decref(seg); free(all);
    char hex[41]; git_oid_tostr(hex, sizeof(hex), &parent);

    /* Incremental fetch across the checkpoint, then a second cold clone. */
    char dest[PATH_MAX], uri[160];
    path_of(dest, sizeof(dest), "clone");
    git_repository *repo = NULL;
    OK(git_repository_open(&repo, dest));
    hanami_config_t *config = NULL;
    OK(hanami_config_new(&config));
    hanami_config_set_allow_insecure(config, true);
    OK(hanami_fetch(repo, NULL, NULL, config));
    assert_ref(repo, "refs/remotes/origin/main", hex);
    git_repository_free(repo);
    hanami_config_free(config);
    assert(gets_of(head) == 1);
    clone_uri(uri, sizeof(uri));
    path_of(dest, sizeof(dest), "clone-after-checkpoint");
    config = fixture_config(NULL);
    OK(hanami_clone(&repo, uri, dest, NULL, config));
    assert_ref(repo, "HEAD", hex);
    git_repository_free(repo);
    hanami_config_free(config);
    assert(gets_of(head) == 2);
    free(head);
    git_repository_free(src);
    printf("checkpoint: 17th link became a full checkpoint; fetch and cold clone read one segment\n");
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

/* Refs-only publication and a tagless republish by another NIP-34 client. */
static void test_republish(void)
{
    const char *relays[] = { relay, NULL };
    char main_now[41], uri[160], dest[PATH_MAX];
    published_main(main_now);
    clone_uri(uri, sizeof(uri));

    /* hanami_publish_state carries the tag, but only for covered targets. */
    hanami_ref_entry_t bad = { "refs/heads/main", "1111111111111111111111111111111111111111" };
    assert(hanami_publish_state("repo", &bad, 1, relays, &signer) == HANAMI_ERR_INDEX);
    hanami_ref_entry_t good = { "refs/heads/main", main_now };
    OK(hanami_publish_state("repo", &good, 1, relays, &signer));
    path_of(dest, sizeof(dest), "clone-after-refs-only");
    hanami_config_t *config = fixture_config(NULL);
    git_repository *repo = NULL;
    OK(hanami_clone(&repo, uri, dest, NULL, config));
    assert_ref(repo, "refs/remotes/origin/main", main_now);
    git_repository_free(repo);

    /* Another client republishes 30618 without the tag: visible failure. */
    publish_state("repo", main_now, NULL, (int64_t)time(NULL) + 1000);
    expect_clone_failure("repo", HANAMI_ERR_NOT_FOUND, "hanami-manifest");

    /* The next hanami push has no base, so it writes a full checkpoint. */
    char p[PATH_MAX]; path_of(p, sizeof(p), "source");
    git_repository *src = NULL;
    OK(git_repository_open(&src, p));
    git_oid parent, next; OK(git_oid_fromstr(&parent, main_now));
    make_commit(src, "after republish\n", "recovered\n", &parent, &next);
    OK(push_source(src));
    char *depth = cache_meta("source", "manifest_depth"), *head = cache_meta("source", "manifest_head");
    assert(!strcmp(depth, "1"));
    json_t *seg = peek_segment(head);
    assert(json_is_null(json_object_get(seg, "prev")));
    json_decref(seg);
    char hex[41]; git_oid_tostr(hex, sizeof(hex), &next);
    path_of(dest, sizeof(dest), "clone-after-republish");
    OK(hanami_clone(&repo, uri, dest, NULL, config));
    assert_ref(repo, "HEAD", hex);
    git_repository_free(repo);
    hanami_config_free(config);
    free(depth); free(head);
    git_repository_free(src);
    printf("republish: refs-only publish guarded; tagless 30618 fails clone; next push checkpoints\n");
}

/* =========================================================================
 * nostrc-sp8zk: CLOSED / AUTH on the kind-10063 and kind-30618 reads.
 * clone_fixture.py relay paths refuse only the listed kinds, so the 30617
 * announcement read succeeds and each failure is pinned to one query.
 * ========================================================================= */

static void announce_on(const char *repo, const char **relays)
{
    const char *clones[] = { grasp, NULL };
    OK(hanami_announce_repo(repo, repo, "clone fixture", clones, relays, &signer));
}

/* Announce on relays; manifest + tagged 30618 for commit-1 (shared store). */
static void seed_repo(const char *repo, const char **relays)
{
    char *c1 = read_text("commit-1");
    char p[PATH_MAX]; path_of(p, sizeof(p), "source");
    git_repository *src = NULL;
    OK(git_repository_open(&src, p));
    blossom_client_t *c = publisher();
    rows_t rows = {0};
    char head[65];
    upload_objects(c, src, NULL, &rows);
    upload_manifest(c, repo, NULL, &rows, head);
    announce_on(repo, relays);
    publish_state(repo, c1, head, (int64_t)time(NULL));
    free(rows.v); free(c1);
    blossom_client_free(c);
    git_repository_free(src);
}

static hanami_error_t clone_via(const char *repo_id, const char *relay_url, const char *endpoint,
                                const hanami_signer_t *who, const char *dir, git_repository **repo)
{
    char uri[200], dest[PATH_MAX];
    snprintf(uri, sizeof(uri), "nostr://%s/%s", pk, repo_id);
    path_of(dest, sizeof(dest), dir);
    hanami_config_t *config = fixture_config_on(relay_url, endpoint);
    *repo = NULL;
    hanami_error_t err = hanami_clone(repo, uri, dest, who, config);
    hanami_config_free(config);
    if (err != HANAMI_OK) assert(!*repo && access(dest, F_OK) != 0);
    return err;
}

static void expect_error(const char *what, hanami_error_t got, hanami_error_t want, const char *needle)
{
    const git_error *e = git_error_last();
    if (got != want || !e || !strstr(e->message, needle)) {
        fprintf(stderr, "%s: got %d (%s), want %d containing '%s'\n", what, got,
                e ? e->message : "", want, needle);
        abort();
    }
}

static void assert_cloned(git_repository *repo, const char *head)
{
    assert(repo);
    assert_ref(repo, "HEAD", head);
    char *selected = repo_config_string(repo, "hanami.endpoint");
    assert(selected && strcmp(selected, blossom) == 0);
    free(selected);
}

static void test_relayfail(void)
{
    char closed_list[200], restricted_list[200], closed_state[200], restricted_state[200];
    char auth_both[200], closed_both[200];
    snprintf(closed_list, sizeof(closed_list), "%s/closed/10063", relay);
    snprintf(restricted_list, sizeof(restricted_list), "%s/restricted/10063", relay);
    snprintf(closed_state, sizeof(closed_state), "%s/closed/30618", relay);
    snprintf(restricted_state, sizeof(restricted_state), "%s/restricted/30618", relay);
    snprintf(auth_both, sizeof(auth_both), "%s/auth/10063-30618", relay);
    snprintf(closed_both, sizeof(closed_both), "%s/closed/10063-30618", relay);
    char *c1 = read_text("commit-1");
    git_repository *repo = NULL;

    /* 1. kind-10063 refused by the only relay: the query error surfaces (not
     *    HANAMI_ERR_CONFIG "no Blossom server known"). */
    const char *r_closed_list[] = { closed_list, NULL };
    announce_on("rf-list-closed", r_closed_list);
    expect_error("10063 CLOSED", clone_via("rf-list-closed", closed_list, NULL, NULL,
                 "rf-list-closed", &repo), HANAMI_ERR_NOSTR, "kind-10063");
    const char *r_restricted_list[] = { restricted_list, NULL };
    announce_on("rf-list-restricted", r_restricted_list);
    expect_error("10063 restricted", clone_via("rf-list-restricted", restricted_list, NULL, NULL,
                 "rf-list-restricted", &repo), HANAMI_ERR_AUTH, "kind-10063");

    /* 2. kind-30618 refused (configured endpoint, so no 10063 read): the
     *    clone fails visibly instead of producing an empty repository. */
    const char *r_closed_state[] = { closed_state, NULL };
    seed_repo("rf-state-closed", r_closed_state);
    expect_error("30618 CLOSED", clone_via("rf-state-closed", closed_state, blossom, NULL,
                 "rf-state-closed", &repo), HANAMI_ERR_NOSTR, "kind-30618");
    const char *r_restricted_state[] = { restricted_state, NULL };
    seed_repo("rf-state-restricted", r_restricted_state);
    expect_error("30618 restricted", clone_via("rf-state-restricted", restricted_state, blossom,
                 NULL, "rf-state-restricted", &repo), HANAMI_ERR_AUTH, "kind-30618");

    /* 3. auth-required: without a signer AUTH fails visibly; with one, each
     *    refusing connection is authenticated once and the clone succeeds. */
    const char *r_auth[] = { auth_both, NULL };
    seed_repo("rf-auth", r_auth);
    long before = counter("auth-ok");
    expect_error("auth, no signer", clone_via("rf-auth", auth_both, NULL, NULL,
                 "rf-auth-nosigner", &repo), HANAMI_ERR_AUTH, "kind-10063");
    assert(counter("auth-ok") == before);
    OK(clone_via("rf-auth", auth_both, NULL, &signer, "rf-auth", &repo));
    assert_cloned(repo, c1);
    assert(counter("auth-ok") >= before + 2); /* the 10063 and the 30618 connection */
    git_repository_free(repo);

    /* 4. Mixed: the configured relay CLOSEs both reads, the announced relay
     *    answers. One relay's CLOSED is that relay's failure only. */
    const char *r_mixed[] = { closed_both, relay, NULL };
    seed_repo("rf-mixed", r_mixed);
    OK(clone_via("rf-mixed", closed_both, NULL, NULL, "rf-mixed", &repo));
    assert_cloned(repo, c1);

    /* 5. hanami_fetch reports the 30618 refusal; mixed relays still fetch. */
    hanami_config_t *config = fixture_config_on(closed_state, NULL);
    expect_error("fetch CLOSED", hanami_fetch(repo, NULL, NULL, config), HANAMI_ERR_NOSTR,
                 "kind-30618");
    hanami_config_free(config);
    config = fixture_config_on(restricted_state, NULL);
    expect_error("fetch restricted", hanami_fetch(repo, NULL, NULL, config), HANAMI_ERR_AUTH,
                 "kind-30618");
    hanami_config_free(config);
    /* Recorded hanami.relays (configured ∪ announced) keep the mixed set. */
    OK(hanami_config_new(&config));
    hanami_config_set_allow_insecure(config, true);
    OK(hanami_fetch(repo, NULL, NULL, config));
    hanami_config_free(config);
    assert_ref(repo, "refs/remotes/origin/main", c1);
    git_repository_free(repo);
    free(c1);
    printf("relayfail: 10063/30618 CLOSED -> NOSTR, restricted/auth-required -> AUTH through "
           "clone and fetch; AUTH retry succeeds; one CLOSED relay does not abort the others\n");
}

/* =========================================================================
 * nostrc-nd9yl: the owner's kind-10063 and kind-30618 live only on their
 * NIP-65 write relay (an isolated fixture store nothing else mirrors).
 * ========================================================================= */

static const char *sk2 = "0000000000000000000000000000000000000000000000000000000000000002";

static void publish_as(const hanami_signer_t *who, const char *key, NostrEvent *ev,
                       const char *url, int64_t at)
{
    nostr_event_set_created_at(ev, at);
    assert(nostr_event_sign(ev, key) == 0);
    const char *relays[] = { url, NULL };
    hanami_nostr_ctx_t *ctx = NULL;
    OK(hanami_nostr_ctx_new(relays, who, &ctx));
    OK(hanami_nostr_publish_event(ctx, ev));
    hanami_nostr_ctx_free(ctx);
    nostr_event_free(ev);
}

static NostrEvent *tagged_event(int kind, const char *const (*tags)[3], size_t n)
{
    NostrEvent *ev = nostr_event_new(); assert(ev);
    nostr_event_set_kind(ev, kind);
    nostr_event_set_content(ev, "");
    NostrTags *t = nostr_tags_new(0); assert(t);
    for (size_t i = 0; i < n; i++)
        nostr_tags_append(t, tags[i][2] ? nostr_tag_new(tags[i][0], tags[i][1], tags[i][2], NULL)
                                        : nostr_tag_new(tags[i][0], tags[i][1], NULL));
    nostr_event_set_tags(ev, t);
    return ev;
}

static void test_outbox(void)
{
    char *pk2 = nostr_key_get_public(sk2); assert(pk2);
    hanami_signer_t signer2 = { .pubkey = pk2, .sign = sign_event, .user_data = (void *)sk2 };
    char outbox[200], outbox_slash[210], readonly[200], stale[200], filler[4][200];
    snprintf(outbox, sizeof(outbox), "%s/isolated/outbox", relay);
    snprintf(outbox_slash, sizeof(outbox_slash), "%s/", outbox);
    snprintf(readonly, sizeof(readonly), "%s/isolated/readonly", relay);
    snprintf(stale, sizeof(stale), "%s/isolated/stale", relay);
    for (int i = 0; i < 4; i++)
        snprintf(filler[i], sizeof(filler[i]), "%s/isolated/filler%d", relay, i);
    int64_t now = (int64_t)time(NULL);

    /* Announcement on the discovery relay; 10063 and tagged 30618 only on
     * the owner's outbox relay. */
    const char *relays[] = { relay, NULL };
    const char *clones[] = { grasp, NULL };
    OK(hanami_announce_repo("obx", "obx", "outbox fixture", clones, relays, &signer2));
    const char *const servers[][3] = { { "server", blossom, NULL } };
    publish_as(&signer2, sk2, tagged_event(NOSTR_KIND_BLOSSOM_SERVER_LIST, servers, 1), outbox, now);
    char *c1 = read_text("commit-1");
    char p[PATH_MAX]; path_of(p, sizeof(p), "source");
    git_repository *src = NULL;
    OK(git_repository_open(&src, p));
    blossom_client_t *c = publisher();
    rows_t rows = {0};
    char head[65];
    upload_objects(c, src, NULL, &rows);
    upload_manifest(c, "obx", NULL, &rows, head);
    nip34_ref_t ref = { (char *)"refs/heads/main", c1 };
    NostrEvent *state = nip34_create_repo_state("obx", &ref, 1, "ref: refs/heads/main");
    assert(state);
    nostr_tags_append(state->tags, nostr_tag_new("hanami-manifest", head, NULL));
    publish_as(&signer2, sk2, state, outbox, now);

    char uri[200], dest[PATH_MAX];
    snprintf(uri, sizeof(uri), "nostr://%s/obx", pk2);
    git_repository *repo = NULL;

    /* 1. No kind-10002: no extra relays (not an error), so nothing names a
     *    Blossom server and the clone fails visibly as before. */
    hanami_config_t *config = fixture_config(NULL);
    path_of(dest, sizeof(dest), "obx-no-10002");
    expect_error("no 10002", hanami_clone(&repo, uri, dest, NULL, config), HANAMI_ERR_CONFIG,
                 "no Blossom server known");
    assert(!repo && access(dest, F_OK) != 0);
    hanami_config_free(config);

    /* 2. kind-10002 on the discovery relay; an older list on a second
     *    configured relay loses (newest replaceable event wins). Read-only,
     *    duplicate, non-ws and over-cap entries are not added. */
    const char *const old_list[][3] = { { "r", readonly, "write" } };
    publish_as(&signer2, sk2, tagged_event(NOSTR_KIND_RELAY_LIST_METADATA, old_list, 1), stale, now - 100);
    const char *const list[][3] = {
        { "r", readonly, "read" },
        { "r", relay, NULL },                 /* already present: no slot used */
        { "r", outbox, NULL },
        { "r", outbox_slash, "write" },       /* same relay */
        { "r", "https://not-a-relay.invalid", NULL },
        { "r", filler[0], "write" },
        { "r", filler[1], NULL },
        { "r", filler[2], "write" },
        { "r", filler[3], "write" },          /* beyond HANAMI_OUTBOX_RELAYS_MAX */
    };
    publish_as(&signer2, sk2, tagged_event(NOSTR_KIND_RELAY_LIST_METADATA, list, 9), relay, now);
    const char *two[] = { relay, stale, NULL };
    OK(hanami_config_new(&config));
    OK(hanami_config_set_relays(config, two, 2));
    hanami_config_set_allow_insecure(config, true);
    path_of(dest, sizeof(dest), "obx");
    OK(hanami_clone(&repo, uri, dest, NULL, config));
    hanami_config_free(config);
    assert_cloned(repo, c1);
    char *recorded = repo_config_string(repo, "hanami.relays");
    assert(recorded);
    char expect[2048];
    snprintf(expect, sizeof(expect), "%s,%s,%s,%s,%s,%s", relay, stale, outbox,
             filler[0], filler[1], filler[2]);
    if (strcmp(recorded, expect) != 0) {
        fprintf(stderr, "hanami.relays: got %s\nwant %s\n", recorded, expect);
        abort();
    }
    free(recorded);

    /* 3. hanami_fetch keeps reading the outbox relay from hanami.relays. */
    OK(hanami_config_new(&config));
    hanami_config_set_allow_insecure(config, true);
    OK(hanami_fetch(repo, NULL, NULL, config));
    hanami_config_free(config);
    assert_ref(repo, "refs/remotes/origin/main", c1);
    git_repository_free(repo);

    free(rows.v); free(c1); free(pk2);
    blossom_client_free(c);
    git_repository_free(src);
    printf("outbox: kind-10063/30618 found via the owner's NIP-65 write relays (newest list, "
           "write-only, deduplicated, capped); no 10002 adds nothing\n");
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
    else if (!strcmp(mode, "pushfail")) test_pushfail();
    else if (!strcmp(mode, "checkpoint")) test_checkpoint();
    else if (!strcmp(mode, "republish")) test_republish();
    else if (!strcmp(mode, "negative")) test_negative();
    else if (!strcmp(mode, "relayfail")) test_relayfail();
    else if (!strcmp(mode, "outbox")) test_outbox();
    else { fprintf(stderr, "unknown mode %s\n", mode); return 2; }

    free(pk);
    curl_global_cleanup();
    hanami_shutdown();
    git_libgit2_shutdown();
    return 0;
}
