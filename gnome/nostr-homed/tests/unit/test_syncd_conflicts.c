/* nostrc-h10m.6 — conflict resolution inside nostr-home-syncd.
 *
 * Personal-home journal: record -> project -> resolve (each kind/choice)
 * in a temporary HOME -> the resolution edits HOME and enqueues the
 * batcher (what the pusher publishes) -> stays pending until the snapshot
 * reflects the published generation -> commits. Plus the request inbox
 * contract against the CLI writer, and the status writer's new fields. */
#define _GNU_SOURCE
#include "nh_porthome_conflicts.h"
#include "nh_syncd.h"
#include "nh_syncd_conflicts.h"
#include "nh_syncd_status.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern int nh_syncd_state_upsert_file_(nh_syncd_state *, const char *, uint32_t, uint32_t,
                                       uint32_t, uint64_t, uint64_t, const char *,
                                       const char *const *, size_t);
extern int nh_syncd_state_delete_(nh_syncd_state *, const char *);

static char g_root[64], g_home[128], g_state[128];
static uint64_t g_now;

static uint64_t now_fn(void *ud) { (void)ud; return g_now; }

static int rm_cb(const char *p, const struct stat *s, int t, struct FTW *f) {
    (void)s; (void)t; (void)f;
    return remove(p);
}

static void put(const char *rel, const char *body) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", g_home, rel);
    char *slash = strrchr(p, '/');
    *slash = 0;
    char cmd[600];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", p);
    assert(system(cmd) == 0);
    *slash = '/';
    FILE *f = fopen(p, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
}

static int exists(const char *rel) {
    char p[512];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", g_home, rel);
    return lstat(p, &st) == 0;
}

static char *slurp(const char *rel) {
    static char buf[256];
    char p[512];
    snprintf(p, sizeof p, "%s/%s", g_home, rel);
    FILE *f = fopen(p, "r");
    if (!f) return NULL;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

/* Pretend the pusher published `rel` with new content. */
static void track(nh_syncd_state *s, const char *rel, const char *chunk) {
    const char *chunks[] = {chunk};
    assert(nh_syncd_state_upsert_file_(s, rel, 0644, 1000, 1000, 1, 5, "", chunks, 1) == 0);
}

typedef struct { const char *path; nh_syncd_change_kind kind; } change;

static size_t drain(nh_syncd_batcher *b, change *out, size_t cap) {
    nh_syncd_batch *batch = nh_syncd_batcher_take(b, true);
    size_t n = 0;
    for (size_t i = 0; batch && i < nh_syncd_batch_len(batch) && n < cap; ++i) {
        const char *p = NULL;
        nh_syncd_change_kind k;
        assert(!nh_syncd_batch_at(batch, i, &p, &k));
        out[n].path = strdup(p);
        out[n++].kind = k;
    }
    nh_syncd_batch_free(batch);
    return n;
}

static int has_change(const change *c, size_t n, const char *path, nh_syncd_change_kind k) {
    for (size_t i = 0; i < n; ++i)
        if (!strcmp(c[i].path, path) && c[i].kind == k) return 1;
    return 0;
}

static void free_changes(change *c, size_t n) {
    for (size_t i = 0; i < n; ++i) free((char *)c[i].path);
}

static json_t *project(const nh_syncd_state *s) {
    char *wire = NULL;
    int hfd = open(g_home, O_RDONLY | O_DIRECTORY);
    assert(hfd >= 0 && nh_syncd_home_conflicts_project(g_state, hfd, s, &wire) == 0);
    close(hfd);
    json_t *j = json_loads(wire, 0, NULL);
    free(wire);
    assert(json_is_array(j));
    return j;
}

static json_t *row_for(json_t *rows, const char *path) {
    size_t i;
    json_t *r;
    json_array_foreach(rows, i, r)
        if (!strcmp(json_string_value(json_object_get(r, "path")), path)) return r;
    return NULL;
}

static const char *id_of(json_t *rows, const char *path, char out[33]) {
    json_t *r = row_for(rows, path);
    assert(r);
    snprintf(out, 33, "%s", json_string_value(json_object_get(r, "id")));
    return out;
}

static int g_done;
static void done_cb(void *ud, const char *request, const char *id, const char *choice) {
    (void)ud; (void)id; (void)choice;
    assert(request && strlen(request) == 32);
    g_done++;
}

#define REQ(c) c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c c

static void test_personal(void) {
    uint8_t root_id[32] = {1};
    nh_syncd_state *s = NULL;
    assert(nh_syncd_state_new(g_home, "d", "", root_id, &s) == 0);
    nh_syncd_batcher *b = NULL;
    assert(nh_syncd_batcher_new(now_fn, NULL, 0, 0, &b) == 0);
    int hfd = open(g_home, O_RDONLY | O_DIRECTORY);
    assert(hfd >= 0);

    /* Three edit conflicts (one per choice) and both deletion kinds. */
    const char *copy_a = "a.txt.conflict-laptop-20261028T101500Z";
    const char *copy_b = "docs/b.md.conflict-laptop-20261028T101500Z";
    const char *copy_c = "c.txt.conflict-laptop-20261028T101500Z";
    put("a.txt", "remote-a"); put(copy_a, "local-a");
    put("docs/b.md", "remote-b"); put(copy_b, "local-b");
    put("c.txt", "local-c"); put(copy_c, "remote-c");
    put("d.txt", "local-d"); put("d.txt.conflict-laptop-1.conflict-deleted", "");
    put("e.txt", "remote-e"); put("e.txt.conflict-laptop-1.conflict-locally-deleted", "");
    track(s, "a.txt", "aa"); track(s, "docs/b.md", "bb"); track(s, "c.txt", "cc");
    track(s, "d.txt", "dd"); track(s, "e.txt", "ee");
    /* a's copy already synced (published); the other copies never did. */
    track(s, copy_a, "ca");
    const nh_home_conflict_record recs[] = {
        {"a.txt", NH_HOME_CONFLICT_EDIT, copy_a, true},
        {"docs/b.md", NH_HOME_CONFLICT_EDIT, copy_b, true},
        {"c.txt", NH_HOME_CONFLICT_EDIT, copy_c, false},
        {"d.txt", NH_HOME_CONFLICT_DELETED_REMOTELY, "d.txt.conflict-laptop-1.conflict-deleted", false},
        {"e.txt", NH_HOME_CONFLICT_DELETED_LOCALLY, "e.txt.conflict-laptop-1.conflict-locally-deleted", false},
    };
    assert(nh_syncd_home_conflicts_record(g_state, recs, 5, 42, 1793000000) == 0);

    json_t *rows = project(s);
    assert(json_array_size(rows) == 5);
    json_t *a = row_for(rows, "a.txt");
    assert(!strcmp(json_string_value(json_object_get(a, "kind")), "edit"));
    assert(!strcmp(json_string_value(json_object_get(a, "destination")), copy_a));
    assert(!strcmp(json_string_value(json_object_get(a, "mine")), "saved"));
    assert(json_array_size(json_object_get(a, "choices")) == 3);
    assert(json_integer_value(json_object_get(a, "generation")) == 42);
    json_t *c = row_for(rows, "c.txt");
    assert(!strcmp(json_string_value(json_object_get(c, "mine")), "current"));
    json_t *d = row_for(rows, "d.txt");
    assert(json_is_false(json_object_get(d, "present")) && json_object_get(d, "marker"));
    assert(json_array_size(json_object_get(d, "choices")) == 2);
    assert(!strcmp(json_string_value(json_object_get(row_for(rows, "e.txt"), "mine")), "saved"));

    char ida[33], idb[33], idc[33], idd[33], ide[33];
    id_of(rows, "a.txt", ida); id_of(rows, "docs/b.md", idb); id_of(rows, "c.txt", idc);
    id_of(rows, "d.txt", idd); id_of(rows, "e.txt", ide);
    char token_a[65];
    snprintf(token_a, sizeof token_a, "%s", json_string_value(json_object_get(a, "expect")));
    json_decref(rows);

    const char *why = NULL;
    /* Refusals: unknown id, keep-both on a deletion, stale token. */
    assert(nh_syncd_home_conflicts_resolve(g_state, hfd, s, b, "ffffffffffffffffffffffffffffffff",
                                           NH_CONFLICT_KEEP_CURRENT, "", REQ("1"), &why) < 0);
    assert(!strcmp(why, "not-found"));
    assert(nh_syncd_home_conflicts_resolve(g_state, hfd, s, b, idd, NH_CONFLICT_KEEP_BOTH, "",
                                           REQ("2"), &why) < 0);
    assert(!strcmp(why, "unsupported-choice"));
    assert(nh_syncd_home_conflicts_resolve(g_state, hfd, s, b, ida, NH_CONFLICT_USE_SAVED,
                                           "0000", REQ("3"), &why) < 0);
    assert(!strcmp(why, "stale"));

    /* use-saved: the saved local edit replaces the remote winner. */
    assert(nh_syncd_home_conflicts_resolve(g_state, hfd, s, b, ida, NH_CONFLICT_USE_SAVED,
                                           token_a, REQ("4"), &why) == NH_CONFLICT_QUEUED);
    assert(!strcmp(slurp("a.txt"), "local-a") && !exists(copy_a));
    /* Same request again is idempotent; a different one is "busy". */
    assert(nh_syncd_home_conflicts_resolve(g_state, hfd, s, b, ida, NH_CONFLICT_USE_SAVED,
                                           "", REQ("4"), &why) == NH_CONFLICT_QUEUED);
    assert(nh_syncd_home_conflicts_resolve(g_state, hfd, s, b, ida, NH_CONFLICT_KEEP_CURRENT,
                                           "", REQ("5"), &why) < 0 && !strcmp(why, "busy"));
    change ch[16];
    size_t n = drain(b, ch, 16);
    assert(has_change(ch, n, "a.txt", NH_SYNCD_CHANGE_MODIFY));
    assert(has_change(ch, n, copy_a, NH_SYNCD_CHANGE_DELETE));
    free_changes(ch, n);

    /* keep-both: the saved copy moves to a readable sibling name. */
    assert(nh_syncd_home_conflicts_resolve(g_state, hfd, s, b, idb, NH_CONFLICT_KEEP_BOTH, "",
                                           REQ("6"), &why) == NH_CONFLICT_QUEUED);
    const char *sib = "docs/b (conflicted copy 2026-10-26).md"; /* detected_at 1793000000 */
    assert(!strcmp(slurp("docs/b.md"), "remote-b") && !strcmp(slurp(sib), "local-b"));
    assert(!exists(copy_b));
    n = drain(b, ch, 16);
    assert(has_change(ch, n, sib, NH_SYNCD_CHANGE_CREATE));
    assert(has_change(ch, n, copy_b, NH_SYNCD_CHANGE_DELETE));
    free_changes(ch, n);

    /* keep-current: only the saved copy goes. It never synced, so there is
     * nothing to publish and the row resolves at once. */
    assert(nh_syncd_home_conflicts_resolve(g_state, hfd, s, b, idc, NH_CONFLICT_KEEP_CURRENT, "",
                                           REQ("7"), &why) == 0);
    assert(!strcmp(slurp("c.txt"), "local-c") && !exists(copy_c));

    /* Deletions: delete the kept file (and marker) / keep it (marker only). */
    assert(nh_syncd_home_conflicts_resolve(g_state, hfd, s, b, idd, NH_CONFLICT_USE_SAVED, "",
                                           REQ("8"), &why) == NH_CONFLICT_QUEUED);
    assert(!exists("d.txt") && !exists("d.txt.conflict-laptop-1.conflict-deleted"));
    assert(nh_syncd_home_conflicts_resolve(g_state, hfd, s, b, ide, NH_CONFLICT_KEEP_CURRENT, "",
                                           REQ("9"), &why) == 0);
    assert(exists("e.txt") && !exists("e.txt.conflict-laptop-1.conflict-locally-deleted"));
    n = drain(b, ch, 16);
    assert(has_change(ch, n, "d.txt", NH_SYNCD_CHANGE_DELETE));
    free_changes(ch, n);

    /* Still listed as pending until a push publishes the touched paths. */
    rows = project(s);
    assert(json_array_size(rows) == 3);
    assert(!strcmp(json_string_value(json_object_get(row_for(rows, "a.txt"), "state")), "pending"));
    json_decref(rows);
    g_done = 0;
    assert(nh_syncd_home_conflicts_commit_pending(g_state, s, done_cb, NULL) == 0 && g_done == 0);

    /* Simulate the pusher's snapshot after a successful generation:
     * a.txt has new content, copies/markers are gone, d.txt deleted. */
    track(s, "a.txt", "a2");
    track(s, sib, "sib");
    nh_syncd_state_delete_(s, "d.txt");
    assert(nh_syncd_home_conflicts_commit_pending(g_state, s, done_cb, NULL) == 0);
    assert(g_done == 2); /* b and d published; a's synced copy is not deleted yet */
    nh_syncd_state_delete_(s, copy_a);
    assert(nh_syncd_home_conflicts_commit_pending(g_state, s, done_cb, NULL) == 0);
    assert(g_done == 3);
    rows = project(s);
    assert(json_array_size(rows) == 0);
    json_decref(rows);
    /* A resolved row stays resolved for its request; others are refused. */
    assert(nh_syncd_home_conflicts_resolve(g_state, hfd, s, b, ida, NH_CONFLICT_USE_SAVED, "",
                                           REQ("4"), &why) == 0);
    assert(nh_syncd_home_conflicts_resolve(g_state, hfd, s, b, ida, NH_CONFLICT_USE_SAVED, "",
                                           REQ("a"), &why) < 0 && !strcmp(why, "already-resolved"));

    /* A saved copy deleted by hand cannot be used, and nothing changes. */
    put("f.txt", "f"); put("f.txt.conflict-x-1", "gone-soon");
    const nh_home_conflict_record rf = {"f.txt", NH_HOME_CONFLICT_EDIT, "f.txt.conflict-x-1", true};
    assert(nh_syncd_home_conflicts_record(g_state, &rf, 1, 43, 1793000001) == 0);
    rows = project(s);
    char idf[33];
    id_of(rows, "f.txt", idf);
    json_decref(rows);
    char p[512];
    snprintf(p, sizeof p, "%s/f.txt.conflict-x-1", g_home);
    unlink(p);
    assert(nh_syncd_home_conflicts_resolve(g_state, hfd, s, b, idf, NH_CONFLICT_USE_SAVED, "",
                                           REQ("b"), &why) < 0);
    assert(!strcmp(why, "saved-copy-missing") && !strcmp(slurp("f.txt"), "f"));
    rows = project(s);
    assert(!strcmp(json_string_value(json_object_get(row_for(rows, "f.txt"), "state")), "active"));
    json_decref(rows);

    close(hfd);
    nh_syncd_batcher_free(b);
    nh_syncd_state_free(s);
}

static void test_inbox(void) {
    char dir[256];
    snprintf(dir, sizeof dir, "%s/requests", g_root);
    nh_syncd_requests *rq = NULL;
    assert(nh_syncd_requests_open(dir, &rq) == 0);
    assert(nh_syncd_requests_fd(rq) >= 0);
    nh_syncd_request req;
    char bad[33];
    assert(nh_syncd_requests_next(rq, &req, bad) == 0);

    /* What the CLI writes is exactly what syncd accepts. */
    const char *vault_id = "abababababababababababababababababababababababababababababababab";
    char rid[33];
    assert(nh_porthome_request_write(dir, vault_id, NH_CONFLICT_CHOICE_USE_SAVED, "tok", rid) == 0);
    assert(nh_syncd_requests_next(rq, &req, bad) == 1);
    assert(!strcmp(req.request, rid) && !strcmp(req.conflict, vault_id));
    assert(req.choice == NH_CONFLICT_USE_SAVED && !strcmp(req.expect, "tok"));
    nh_syncd_requests_done(rq, req.request);
    assert(nh_syncd_requests_next(rq, &req, bad) == 0);

    /* Malformed: wrong id in body, unknown op, extra field, symlink. */
    const char *bodies[] = {
        "{\"schema\":1,\"op\":\"resolve-conflict\",\"request\":\"" REQ("9") "\",\"conflict\":\"" REQ("1") "\",\"choice\":\"keep-current\",\"expect\":\"\"}",
        "{\"schema\":1,\"op\":\"rm-rf\",\"request\":\"" REQ("1") "\",\"conflict\":\"" REQ("1") "\",\"choice\":\"keep-current\",\"expect\":\"\"}",
        "{\"schema\":1,\"op\":\"resolve-conflict\",\"request\":\"" REQ("1") "\",\"conflict\":\"" REQ("1") "\",\"choice\":\"keep-current\",\"expect\":\"\",\"path\":\"/etc/passwd\"}",
    };
    for (size_t i = 0; i < 3; ++i) {
        char p[512];
        snprintf(p, sizeof p, "%s/%s.json", dir, REQ("1"));
        FILE *f = fopen(p, "w");
        fputs(bodies[i], f);
        fclose(f);
        assert(nh_syncd_requests_next(rq, &req, bad) == 2 && !strcmp(bad, REQ("1")));
        struct stat st;
        assert(stat(p, &st) != 0); /* removed */
    }
    char target[512], link_path[512];
    snprintf(target, sizeof target, "%s/elsewhere.json", g_root);
    FILE *f = fopen(target, "w");
    fputs(bodies[0], f);
    fclose(f);
    snprintf(link_path, sizeof link_path, "%s/%s.json", dir, REQ("2"));
    assert(symlink(target, link_path) == 0);
    assert(nh_syncd_requests_next(rq, &req, bad) == 2);
    struct stat st;
    assert(lstat(link_path, &st) != 0 && stat(target, &st) == 0); /* link gone, target kept */
    nh_syncd_requests_close(rq);
}

static void test_status_fields(void) {
    char path[256];
    snprintf(path, sizeof path, "%s/porthome-status.json", g_root);
    nh_syncd_status_writer *w = nh_syncd_status_writer_new();
    assert(nh_syncd_status_set_home_conflicts(w, "[{\"id\":\"x\"}]") == 0);
    assert(nh_syncd_status_set_vault_conflicts(w, "[{\"id\":\"y\"},{\"id\":\"z\"}]") == 0);
    assert(nh_syncd_status_set_home_conflicts(w, "not json") == -1); /* keeps previous */
    nh_syncd_status_record_resolution(w, REQ("1"), "x", "keep-both", "pending", "offline");
    nh_syncd_status_record_resolution(w, REQ("1"), "x", "keep-both", "ok", "");
    assert(nh_syncd_status_emit(w, path) == 0);
    json_t *doc = json_load_file(path, 0, NULL);
    json_t *syncd = json_object_get(doc, "syncd");
    assert(json_integer_value(json_object_get(syncd, "conflicts_active_count")) == 3);
    assert(json_array_size(json_object_get(syncd, "home_conflicts")) == 1);
    json_t *res = json_object_get(syncd, "resolutions");
    assert(json_array_size(res) == 2);
    assert(!strcmp(json_string_value(json_object_get(json_array_get(res, 0), "result")), "ok"));
    /* The CLI's lookup agrees (newest first). */
    char *wire = json_dumps(doc, 0), result[16], reason[32];
    assert(!nh_porthome_resolution_lookup(wire, strlen(wire), REQ("1"), result, reason));
    assert(!strcmp(result, "ok"));
    free(wire);
    json_decref(doc);
    /* The ring is bounded. */
    for (int i = 0; i < 40; ++i) nh_syncd_status_record_resolution(w, REQ("2"), "y", "keep-current", "ok", "");
    assert(nh_syncd_status_emit(w, path) == 0);
    doc = json_load_file(path, 0, NULL);
    assert(json_array_size(json_object_get(json_object_get(doc, "syncd"), "resolutions")) ==
           NH_SYNCD_STATUS_RESOLUTIONS_CAP);
    json_decref(doc);
    nh_syncd_status_writer_free(w);
}

static void test_sibling_names(void) {
    uint8_t root_id[32] = {2};
    nh_syncd_state *s = NULL;
    assert(nh_syncd_state_new(g_home, "d", "", root_id, &s) == 0);
    int hfd = open(g_home, O_RDONLY | O_DIRECTORY);
    char *out = NULL;
    assert(!nh_conflict_sibling_path(hfd, s, "x/notes.txt", "from 3f2a1b0c", &out));
    assert(!strcmp(out, "x/notes (conflicted copy from 3f2a1b0c).txt"));
    free(out);
    put("x/notes (conflicted copy from 3f2a1b0c).txt", "taken");
    assert(!nh_conflict_sibling_path(hfd, s, "x/notes.txt", "from 3f2a1b0c", &out));
    assert(!strcmp(out, "x/notes (conflicted copy from 3f2a1b0c 2).txt"));
    free(out);
    assert(!nh_conflict_sibling_path(hfd, s, ".bashrc", "2026-10-28", &out));
    assert(!strcmp(out, ".bashrc (conflicted copy 2026-10-28)"));
    free(out);
    char longname[300];
    memset(longname, 'n', 250);
    strcpy(longname + 250, ".txt");
    assert(!nh_conflict_sibling_path(hfd, s, longname, "from 3f2a1b0c", &out));
    assert(strlen(out) <= 255 && strstr(out, "(conflicted copy from 3f2a1b0c).txt"));
    free(out);
    assert(nh_conflict_sibling_path(hfd, s, "../escape", "x", &out) != 0);
    close(hfd);
    nh_syncd_state_free(s);
}

int main(void) {
    snprintf(g_root, sizeof g_root, "/tmp/nh-syncd-conflicts-XXXXXX");
    assert(mkdtemp(g_root));
    snprintf(g_home, sizeof g_home, "%s/home", g_root);
    snprintf(g_state, sizeof g_state, "%s/state", g_root);
    assert(mkdir(g_home, 0700) == 0 && mkdir(g_state, 0700) == 0);
    test_personal();
    test_inbox();
    test_status_fields();
    test_sibling_names();
    nftw(g_root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    printf("PASS test_syncd_conflicts\n");
    return 0;
}
