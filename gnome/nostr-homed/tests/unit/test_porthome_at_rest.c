/* SPDX-License-Identifier: MIT */
#define _XOPEN_SOURCE 700
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "nh_porthome_at_rest.h"
#include "nh_porthome_status.h"
#include <assert.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[PATH_MAX];
static void path(char *out, const char *name) {
    int n = snprintf(out, PATH_MAX, "%s/%s", root, name);
    assert(n > 0 && n < PATH_MAX);
}
static void dir(const char *name) {
    char p[PATH_MAX]; path(p, name); assert(mkdir(p, 0700) == 0);
}
static void file(const char *name, const char *body) {
    char p[PATH_MAX]; path(p, name);
    FILE *f = fopen(p, "w"); assert(f); assert(fputs(body, f) >= 0); assert(!fclose(f));
}
static void device(const char *name, const char *uuid, int physical) {
    char p[PATH_MAX];
    dir(name);
    snprintf(p, sizeof p, "%s/slaves", name); dir(p);
    if (physical) { snprintf(p, sizeof p, "%s/device", name); dir(p); }
    if (uuid) {
        snprintf(p, sizeof p, "%s/dm", name); dir(p);
        snprintf(p, sizeof p, "%s/dm/uuid", name); file(p, uuid);
    }
}
static void slave(const char *parent, const char *child) {
    char src[PATH_MAX], dst[PATH_MAX], rel[PATH_MAX];
    path(src, child);
    snprintf(rel, sizeof rel, "%s/slaves/%s", parent, strrchr(child, '/') ? strrchr(child, '/')+1 : child);
    path(dst, rel); assert(!symlink(src, dst));
}
static void expect(const char *name, nh_at_rest expected) {
    char p[PATH_MAX]; path(p, name);
    nh_at_rest actual = nh_porthome_at_rest_device(p);
    printf("%s: %s\n", name, nh_porthome_at_rest_name(actual));
    assert(actual == expected);
}
static int remove_entry(const char *p, const struct stat *s, int type, struct FTW *f) {
    (void)s; (void)type; (void)f; return remove(p);
}
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--probe")) {
        puts(nh_porthome_at_rest_name(nh_porthome_at_rest_path(argv[2])));
        return 0;
    }
    strcpy(root, "/tmp/nh-at-rest-XXXXXX"); assert(mkdtemp(root));
    device("disk", NULL, 1); expect("disk", NH_AT_REST_UNENCRYPTED);
    dir("disk/part1"); file("disk/part1/partition", "1\n");
    expect("disk/part1", NH_AT_REST_UNENCRYPTED);
    device("luks", "CRYPT-LUKS2-fixture\n", 0); slave("luks", "disk/part1");
    expect("luks", NH_AT_REST_ENCRYPTED);
    device("plaincrypt", "CRYPT-PLAIN-fixture\n", 0);
    expect("plaincrypt", NH_AT_REST_ENCRYPTED);
    device("lvm", "LVM-fixture\n", 0); slave("lvm", "disk");
    expect("lvm", NH_AT_REST_UNENCRYPTED);
    device("luks-on-lvm", "CRYPT-LUKS1-fixture\n", 0); slave("luks-on-lvm", "lvm");
    expect("luks-on-lvm", NH_AT_REST_ENCRYPTED);
    device("lvm-on-luks", "LVM-encrypted-pv\n", 0); slave("lvm-on-luks", "luks");
    expect("lvm-on-luks", NH_AT_REST_ENCRYPTED);
    dir("virtual");
    device("virtual/md0", NULL, 0); slave("virtual/md0", "disk");
    expect("virtual/md0", NH_AT_REST_UNENCRYPTED);
    device("virtual/md1", NULL, 0); slave("virtual/md1", "luks"); slave("virtual/md1", "luks-on-lvm");
    expect("virtual/md1", NH_AT_REST_ENCRYPTED);
    device("virtual/md-mixed", NULL, 0); slave("virtual/md-mixed", "luks"); slave("virtual/md-mixed", "disk");
    expect("virtual/md-mixed", NH_AT_REST_UNENCRYPTED);
    device("virtual/loop0", NULL, 0); expect("virtual/loop0", NH_AT_REST_UNKNOWN);
    device("virtual/md-unknown", NULL, 0); slave("virtual/md-unknown", "luks"); slave("virtual/md-unknown", "virtual/loop0");
    expect("virtual/md-unknown", NH_AT_REST_UNKNOWN);
    device("unknown", NULL, 0); expect("unknown", NH_AT_REST_UNKNOWN);
    expect("missing", NH_AT_REST_UNKNOWN);
    dir("unreadable"); expect("unreadable", NH_AT_REST_UNKNOWN);
    device("empty-dm", "LVM-empty\n", 0); expect("empty-dm", NH_AT_REST_UNKNOWN);
    device("bad-uuid", "CRYPT-\n", 0); expect("bad-uuid", NH_AT_REST_UNKNOWN);
    device("unknown-crypt", "CRYPT-FUTURE-fixture\n", 0);
    slave("unknown-crypt", "luks"); expect("unknown-crypt", NH_AT_REST_UNKNOWN);
    device("integrity", "CRYPT-INTEGRITY-fixture\n", 0); slave("integrity", "disk");
    expect("integrity", NH_AT_REST_UNENCRYPTED);
    device("verity", "CRYPT-VERITY-fixture\n", 0); slave("verity", "disk");
    expect("verity", NH_AT_REST_UNENCRYPTED);
    device("integrity-on-luks", "CRYPT-INTEGRITY-fixture\n", 0);
    slave("integrity-on-luks", "luks"); expect("integrity-on-luks", NH_AT_REST_ENCRYPTED);
    dir("luks/part1"); file("luks/part1/partition", "1\n");
    expect("luks/part1", NH_AT_REST_ENCRYPTED);
    device("cycle", "LVM-cycle\n", 0); slave("cycle", "cycle"); expect("cycle", NH_AT_REST_UNKNOWN);
    device("broken", "LVM-broken\n", 0); slave("broken", "absent"); expect("broken", NH_AT_REST_UNKNOWN);
    for (int i = NH_AT_REST_UNKNOWN; i <= NH_AT_REST_ENCRYPTED; i++) {
        assert(nh_porthome_at_rest_allowed(i, 0));
        assert(nh_porthome_at_rest_allowed(i, 1) == (i == NH_AT_REST_ENCRYPTED));
        assert((nh_porthome_at_rest_warning(i) == NULL) == (i == NH_AT_REST_ENCRYPTED));
    }
    assert(nh_porthome_at_rest_fd(-1) == NH_AT_REST_UNKNOWN);
    assert(nh_porthome_at_rest_path(NULL) == NH_AT_REST_UNKNOWN);
    char missing[PATH_MAX]; path(missing, "not-created/home");
    assert(nh_porthome_at_rest_path(missing) == nh_porthome_at_rest_path(root));
    char status[PATH_MAX]; path(status, "status.json");
    assert(!nh_porthome_status_write_key(status, "syncd", "{\"state\":\"idle\"}"));
    assert(!nh_porthome_at_rest_write(status, NH_AT_REST_ENCRYPTED));
    assert(!nh_porthome_at_rest_write(status, NH_AT_REST_UNKNOWN));
    char *body; size_t len;
    assert(!nh_porthome_status_read(status, &body, &len));
    assert(strstr(body, "unknown") && strstr(body, "syncd") && !strstr(body, "encrypted"));
    free(body);
    assert(!nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS));
    puts("PASS: at-rest fixtures, policy, target lookup and status merge");
    return 0;
}
