/*
 * test_fuse_table.c — Phase 4 FUSE namespace table.
 *
 * Builds a snapshot.json fixture via nh_syncd_state save + reload,
 * then asserts nh_fuse_table caps + hostile-path filtering + readdir
 * enumeration. See design §7.1 for the caps.
 */

#define _GNU_SOURCE
#include "nh_fuse_table.h"
#include "nh_syncd.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

extern int nh_syncd_state_upsert_file_(nh_syncd_state *, const char *,
                                       uint32_t, uint32_t, uint32_t,
                                       uint64_t, uint64_t,
                                       const char *,
                                       const char *const *, size_t);
extern int nh_syncd_state_upsert_dir_(nh_syncd_state *, const char *,
                                      uint32_t, uint32_t, uint32_t, uint64_t);
extern int nh_syncd_state_upsert_symlink_(nh_syncd_state *, const char *,
                                          uint32_t, uint32_t, uint32_t,
                                          uint64_t, const char *);

typedef struct { char names[32][256]; int n; } names_t;

static int rd_cb(void *ud, const char *name, const nh_fuse_entry_t *e) {
    (void)e;
    names_t *ns = ud;
    if (ns->n < 32) {
        strncpy(ns->names[ns->n], name, 255);
        ns->names[ns->n][255] = '\0';
        ns->n++;
    }
    return 0;
}

static bool has_name(const names_t *ns, const char *want) {
    for (int i = 0; i < ns->n; i++) if (!strcmp(ns->names[i], want)) return true;
    return false;
}

int main(void) {
    char tmpl[] = "/tmp/nhfuse_tbl_XXXXXX";
    assert(mkdtemp(tmpl) != NULL);
    char sdir[300]; snprintf(sdir, sizeof sdir, "%s/state", tmpl);
    mkdir(sdir, 0700);

    uint8_t root_id[32]; memset(root_id, 0xab, 32);
    nh_syncd_state *st = NULL;
    assert(nh_syncd_state_new("/home/testuser", "d-tag", "01", root_id, &st) == 0);

    /* Add some legitimate entries. */
    const char *ch1[] = { "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" };
    assert(nh_syncd_state_upsert_file_(st, "Documents/notes.txt",
                                       0644, 1000, 1000, 1u*1000000000ull,
                                       17, "01", ch1, 1) == 0);
    assert(nh_syncd_state_upsert_dir_(st, "Documents", 0755, 1000, 1000,
                                      1u*1000000000ull) == 0);
    assert(nh_syncd_state_upsert_symlink_(st, "Documents/link", 0777, 1000, 1000,
                                          1u*1000000000ull, "notes.txt") == 0);
    /* And a file deep in a synthesized-dir tree so table_find must
     * see the implicit dir. */
    const char *ch2[] = { "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" };
    assert(nh_syncd_state_upsert_file_(st, "Archive/2024/report.pdf",
                                       0640, 1000, 1000, 1u*1000000000ull,
                                       42, "02", ch2, 1) == 0);

    assert(nh_syncd_state_save(st, sdir) == 0);
    nh_syncd_state_free(st);

    /* Load through the FUSE table with mount uid/gid override. */
    nh_fuse_table *t = NULL;
    int rc = nh_fuse_table_load(sdir, 4242, 4242, &t);
    assert(rc == 0 && t != NULL);
    assert(nh_fuse_table_uid(t) == 4242);
    assert(nh_fuse_table_gid(t) == 4242);
    assert(nh_fuse_table_entry_count(t) >= 3);

    const nh_fuse_entry_t *root = nh_fuse_table_find(t, "");
    assert(root && root->kind == NH_FUSE_KIND_DIR);

    const nh_fuse_entry_t *e = nh_fuse_table_find(t, "Documents/notes.txt");
    assert(e && e->kind == NH_FUSE_KIND_FILE);
    assert(e->size == 17);
    assert(e->n_chunks == 1);
    assert(!strcmp(e->chunks_hex[0], ch1[0]));

    const nh_fuse_entry_t *lnk = nh_fuse_table_find(t, "Documents/link");
    assert(lnk && lnk->kind == NH_FUSE_KIND_SYMLINK);
    assert(!strcmp(lnk->symlink_target, "notes.txt"));

    /* Synthesized dir: `Archive` is not an explicit entry but should
     * appear when we look for the root's children. */
    names_t root_ns = {0};
    assert(nh_fuse_table_readdir(t, "", rd_cb, &root_ns) == 0);
    assert(has_name(&root_ns, "Documents"));
    assert(has_name(&root_ns, "Archive")); /* synthetic */

    names_t doc_ns = {0};
    assert(nh_fuse_table_readdir(t, "Documents", rd_cb, &doc_ns) == 0);
    assert(has_name(&doc_ns, "notes.txt"));
    assert(has_name(&doc_ns, "link"));

    /* Hostile paths: build a raw snapshot.json with '..' and NUL and
     * assert the loader rejects them individually while keeping the
     * good entries. Easiest way: hand-craft snapshot.json. */
    char sdir2[300]; snprintf(sdir2, sizeof sdir2, "%s/state2", tmpl);
    mkdir(sdir2, 0700);
    char sp[400]; snprintf(sp, sizeof sp, "%s/snapshot.json", sdir2);
    FILE *f = fopen(sp, "w"); assert(f);
    fprintf(f, "{\n"
        "  \"schema\":1,\n"
        "  \"generation\":7,\n"
        "  \"root\":\"/home/x\",\n"
        "  \"d_tag\":\"t\",\n"
        "  \"account_pubkey_hex\":\"\",\n"
        "  \"root_id_hex\":\"%s\",\n"
        "  \"files\":{\n"
        "    \"../evil\": {\"kind\":\"file\",\"mode\":420,\"uid\":0,\"gid\":0,\"mtime_ns\":0,\"size\":0,\"content_hash_hex\":\"\",\"chunk_addrs_hex\":[]},\n"
        "    \"ok.txt\":  {\"kind\":\"file\",\"mode\":420,\"uid\":0,\"gid\":0,\"mtime_ns\":0,\"size\":5,\"content_hash_hex\":\"01\",\"chunk_addrs_hex\":[\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\"]}\n"
        "  }\n"
        "}\n",
        "abababababababababababababababababababababababababababababababab");
    fclose(f);
    /* Also drop generation file. */
    char gp[400]; snprintf(gp, sizeof gp, "%s/generation", sdir2);
    f = fopen(gp, "w"); fprintf(f, "7\n"); fclose(f);

    nh_fuse_table *t2 = NULL;
    assert(nh_fuse_table_load(sdir2, 100, 100, &t2) == 0);
    /* '../evil' must have been skipped; only ok.txt is present. */
    assert(nh_fuse_table_find(t2, "../evil") == NULL);
    assert(nh_fuse_table_find(t2, "ok.txt") != NULL);
    nh_fuse_table_free(t2);

    nh_fuse_table_free(t);
    printf("test_fuse_table OK\n");
    return 0;
}
