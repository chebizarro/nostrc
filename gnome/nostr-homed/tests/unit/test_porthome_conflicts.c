/* nostrc-h10m.6 — reader side of portable-home conflicts.
 *
 * Feeds nh_porthome_conflicts_* a status document shaped like the one
 * nostr-home-syncd writes (syncd.home_conflicts / vault_conflicts /
 * resolutions) and checks normalisation, lookup, choice mapping, the
 * user-facing wording and the request file that syncd's inbox reads. */
#include "nh_porthome_conflicts.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HOME_ID  "0123456789abcdef0123456789abcdef"
#define VAULT_A  "aaaaaaaa11111111111111111111111111111111111111111111111111111111"
#define VAULT_B  "aaaaaaaa22222222222222222222222222222222222222222222222222222222"
#define VAULT_C  "cccccccc33333333333333333333333333333333333333333333333333333333"
#define WRITER   "3f2a1b0c99999999999999999999999999999999999999999999999999999999"

static const char *STATUS =
    "{\"schema\":1,\"syncd\":{\"state\":\"idle\",\"conflicts_active_count\":5,"
    "\"home_conflicts\":["
    "{\"source\":\"home\",\"id\":\"" HOME_ID "\",\"path\":\"Documents/report.txt\","
    "\"kind\":\"edit\",\"present\":true,\"current_present\":true,\"state\":\"active\","
    "\"choices\":[\"keep-current\",\"use-saved\",\"keep-both\"],\"expect\":\"tok-home\","
    "\"destination\":\"Documents/report.txt.conflict-laptop-20261028T101500Z\","
    "\"mine\":\"saved\",\"detected_at\":1793000000,\"generation\":7},"
    /* malformed: id is not hex — skipped, never resolvable */
    "{\"id\":\"zz\",\"path\":\"x\",\"expect\":\"t\"}"
    "],"
    "\"vault_conflicts\":["
    "{\"source\":\"vault\",\"id\":\"" VAULT_A "\",\"vault_id\":\"vv\",\"path\":\"Portable/Team/plan.md\","
    "\"writer\":\"" WRITER "\",\"head\":\"h1\",\"present\":true,\"current_present\":true,"
    "\"state\":\"active\",\"choices\":[\"keep-current\",\"use-saved\",\"keep-both\"],"
    "\"expect\":\"tok-a\",\"overflow\":false,\"destination\":\"Portable/.conflicts/Portable/Team/plan.md@" WRITER "\","
    "\"mine\":\"current\"},"
    "{\"source\":\"vault\",\"id\":\"" VAULT_B "\",\"vault_id\":\"vv\",\"path\":\"Portable/Team/gone.txt\","
    "\"writer\":\"" WRITER "\",\"head\":\"h2\",\"present\":false,\"current_present\":true,"
    "\"state\":\"active\",\"choices\":[\"keep-current\",\"use-saved\"],\"expect\":\"tok-b\","
    "\"overflow\":false,\"mine\":null},"
    "{\"source\":\"vault\",\"id\":\"" VAULT_C "\",\"vault_id\":\"vv\",\"path\":\"Portable/Team/xxxxxxxx\","
    "\"writer\":\"" WRITER "\",\"head\":\"h3\",\"present\":true,\"current_present\":false,"
    "\"state\":\"pending\",\"choices\":[\"keep-current\",\"use-saved\"],\"expect\":\"absent\","
    "\"overflow\":true,\"destination\":\"Portable/.conflicts/.overflow/abcd\",\"mine\":\"saved\"}"
    "],"
    "\"resolutions\":["
    "{\"ts\":3,\"request\":\"r1\",\"conflict\":\"" HOME_ID "\",\"choice\":\"keep-both\",\"result\":\"ok\",\"reason\":\"\"},"
    "{\"ts\":2,\"request\":\"r1\",\"conflict\":\"" HOME_ID "\",\"choice\":\"keep-both\",\"result\":\"pending\",\"reason\":\"offline\"},"
    "{\"ts\":1,\"request\":\"r2\",\"conflict\":\"" VAULT_A "\",\"choice\":\"use-saved\",\"result\":\"error\",\"reason\":\"stale\"}"
    "]}}";

static const nh_porthome_conflict *by_id(const nh_porthome_conflict_list *l, const char *id) {
    int i = nh_porthome_conflicts_find(l, id);
    assert(i >= 0);
    return &l->items[i];
}

static void test_parse(void) {
    nh_porthome_conflict_list l;
    assert(nh_porthome_conflicts_parse(STATUS, strlen(STATUS), &l) == 0);
    assert(l.n == 4); /* the malformed home row is dropped */

    const nh_porthome_conflict *h = by_id(&l, HOME_ID);
    assert(h->source == NH_CONFLICT_SOURCE_HOME);
    assert(!strcmp(h->path, "Documents/report.txt"));
    assert(!strcmp(h->saved_path, "Documents/report.txt.conflict-laptop-20261028T101500Z"));
    assert(!strcmp(h->kind, "edit") && h->saved_present && h->current_present && !h->pending);
    assert(h->mine == NH_CONFLICT_MINE_SAVED && h->detected_at == 1793000000);
    assert(h->choices == (NH_CONFLICT_CHOICE_KEEP_CURRENT | NH_CONFLICT_CHOICE_USE_SAVED |
                          NH_CONFLICT_CHOICE_KEEP_BOTH));
    assert(!strcmp(h->expect, "tok-home"));

    const nh_porthome_conflict *d = by_id(&l, VAULT_B);
    assert(d->source == NH_CONFLICT_SOURCE_VAULT && !d->saved_present && d->current_present);
    assert(!d->saved_path && d->mine == NH_CONFLICT_MINE_UNKNOWN);
    assert(!(d->choices & NH_CONFLICT_CHOICE_KEEP_BOTH));

    const nh_porthome_conflict *o = by_id(&l, VAULT_C);
    assert(o->overflow && o->pending && !o->current_present);
    assert(!strcmp(o->saved_path, "Portable/.conflicts/.overflow/abcd"));

    /* Empty / missing arrays and junk documents. */
    nh_porthome_conflicts_clear(&l);
    assert(nh_porthome_conflicts_parse("{\"schema\":1}", 12, &l) == 0 && l.n == 0);
    nh_porthome_conflicts_clear(&l);
    assert(nh_porthome_conflicts_parse("[1]", 3, &l) == -EINVAL);
    assert(nh_porthome_conflicts_parse("not json", 8, &l) == -EINVAL);
}

static void test_find(void) {
    nh_porthome_conflict_list l;
    assert(nh_porthome_conflicts_parse(STATUS, strlen(STATUS), &l) == 0);
    assert(nh_porthome_conflicts_find(&l, "012345") >= 0);           /* unique prefix */
    assert(nh_porthome_conflicts_find(&l, "aaaaaaaa") == -EEXIST);   /* two vault rows */
    assert(nh_porthome_conflicts_find(&l, "aaaaaaaa1") >= 0);
    assert(nh_porthome_conflicts_find(&l, "ffffff") == -ENOENT);
    assert(nh_porthome_conflicts_find(&l, "01234") == -EINVAL);      /* too short */
    assert(nh_porthome_conflicts_find(&l, "01234Z") == -EINVAL);
    nh_porthome_conflicts_clear(&l);
}

static void test_choices(void) {
    nh_porthome_conflict_list l;
    assert(nh_porthome_conflicts_parse(STATUS, strlen(STATUS), &l) == 0);
    const nh_porthome_conflict *h = by_id(&l, HOME_ID);   /* mine = saved   */
    const nh_porthome_conflict *a = by_id(&l, VAULT_A);   /* mine = current */
    const nh_porthome_conflict *b = by_id(&l, VAULT_B);   /* mine unknown   */
    unsigned c = 0;
    bool alias = false;

    assert(!nh_porthome_conflict_choice_from_flag(h, "--keep-mine", &c, &alias) && alias);
    assert(c == NH_CONFLICT_CHOICE_USE_SAVED);
    assert(!nh_porthome_conflict_choice_from_flag(h, "--take-theirs", &c, &alias));
    assert(c == NH_CONFLICT_CHOICE_KEEP_CURRENT);
    assert(!nh_porthome_conflict_choice_from_flag(a, "--keep-mine", &c, &alias));
    assert(c == NH_CONFLICT_CHOICE_KEEP_CURRENT);
    assert(!nh_porthome_conflict_choice_from_flag(a, "--take-theirs", &c, &alias));
    assert(c == NH_CONFLICT_CHOICE_USE_SAVED);
    /* Unknown provenance: aliases refused, explicit choices fine. */
    assert(nh_porthome_conflict_choice_from_flag(b, "--keep-mine", &c, &alias) == -ENOENT);
    assert(!nh_porthome_conflict_choice_from_flag(b, "--use-saved", &c, &alias) && !alias);
    assert(c == NH_CONFLICT_CHOICE_USE_SAVED);
    assert(nh_porthome_conflict_choice_from_flag(b, "--merge", &c, &alias) == -EINVAL);

    /* Labels say what actually happens, including deletion conflicts. */
    assert(!strcmp(nh_porthome_conflict_choice_label(h, NH_CONFLICT_CHOICE_KEEP_CURRENT),
                   "Keep the current version"));
    assert(!strcmp(nh_porthome_conflict_choice_label(b, NH_CONFLICT_CHOICE_KEEP_CURRENT),
                   "Keep the file"));
    assert(!strcmp(nh_porthome_conflict_choice_label(b, NH_CONFLICT_CHOICE_USE_SAVED),
                   "Delete the file"));
    const nh_porthome_conflict *o = by_id(&l, VAULT_C);
    assert(!strcmp(nh_porthome_conflict_choice_label(o, NH_CONFLICT_CHOICE_USE_SAVED),
                   "Restore the saved version"));
    assert(!strcmp(nh_porthome_conflict_choice_slug(NH_CONFLICT_CHOICE_KEEP_BOTH), "keep-both"));
    nh_porthome_conflicts_clear(&l);
}

static void test_describe(void) {
    nh_porthome_conflict_list l;
    assert(nh_porthome_conflicts_parse(STATUS, strlen(STATUS), &l) == 0);
    char buf[600];
    assert(nh_porthome_conflict_describe(by_id(&l, HOME_ID), buf, sizeof buf) > 0);
    assert(strstr(buf, "changed in two places") && strstr(buf, "saved at Documents/report.txt.conflict-"));
    assert(strstr(buf, "The saved version is yours."));
    assert(nh_porthome_conflict_describe(by_id(&l, VAULT_B), buf, sizeof buf) > 0);
    assert(strstr(buf, "deleted by another member"));
    assert(nh_porthome_conflict_describe(by_id(&l, VAULT_C), buf, sizeof buf) > 0);
    assert(strstr(buf, "another member (3f2a1b0c)") && strstr(buf, ".overflow/abcd"));
    char tiny[8];
    assert(nh_porthome_conflict_describe(by_id(&l, HOME_ID), tiny, sizeof tiny) == -1 && !tiny[0]);
    nh_porthome_conflicts_clear(&l);
}

static void test_resolution_lookup(void) {
    char result[16], reason[32];
    /* Newest entry wins: r1 went pending -> ok. */
    assert(!nh_porthome_resolution_lookup(STATUS, strlen(STATUS), "r1", result, reason));
    assert(!strcmp(result, "ok") && !reason[0]);
    assert(!nh_porthome_resolution_lookup(STATUS, strlen(STATUS), "r2", result, reason));
    assert(!strcmp(result, "error") && !strcmp(reason, "stale"));
    assert(nh_porthome_resolution_lookup(STATUS, strlen(STATUS), "r9", result, reason) == -ENOENT);
    assert(strstr(nh_porthome_resolution_reason_text("stale"), "changed after"));
    assert(strstr(nh_porthome_resolution_reason_text("never-heard-of-it"), "could not"));
}

static void test_request_write(void) {
    char dir[] = "/tmp/nh-conflict-req-XXXXXX";
    assert(mkdtemp(dir));
    char inbox[256];
    snprintf(inbox, sizeof inbox, "%s/requests", dir);
    char req[33];
    assert(nh_porthome_request_write(inbox, VAULT_A, NH_CONFLICT_CHOICE_KEEP_BOTH, "tok-a", req) == 0);
    assert(strlen(req) == 32);
    struct stat st;
    assert(stat(inbox, &st) == 0 && (st.st_mode & 0777) == 0700);

    char path[512];
    snprintf(path, sizeof path, "%s/%s.json", inbox, req);
    assert(stat(path, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600);
    FILE *f = fopen(path, "r");
    char body[1024] = {0};
    assert(f && fread(body, 1, sizeof body - 1, f) > 0);
    fclose(f);
    assert(strstr(body, "\"op\":\"resolve-conflict\"") && strstr(body, "\"choice\":\"keep-both\""));
    assert(strstr(body, "\"conflict\":\"" VAULT_A "\"") && strstr(body, "\"expect\":\"tok-a\""));
    assert(strstr(body, req));

    /* Only the final file remains (no staging leftovers). */
    DIR *d = opendir(inbox);
    int entries = 0;
    struct dirent *de;
    while ((de = readdir(d)))
        if (de->d_name[0] != '.' || (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")))
            ++entries;
    closedir(d);
    assert(entries == 1);

    /* Bad inputs never write. */
    assert(nh_porthome_request_write(inbox, "nothex", NH_CONFLICT_CHOICE_KEEP_CURRENT, "", req) == -EINVAL);
    assert(nh_porthome_request_write(inbox, HOME_ID, 3u, "", req) == -EINVAL);

    unlink(path);
    rmdir(inbox);
    rmdir(dir);
}

int main(void) {
    test_parse();
    test_find();
    test_choices();
    test_describe();
    test_resolution_lookup();
    test_request_write();
    printf("PASS test_porthome_conflicts\n");
    return 0;
}
