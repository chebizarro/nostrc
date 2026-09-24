/*
 * test_syncd_ignore.c — unit tests for the ignore-set matcher (§6.2).
 *
 * SPDX-License-Identifier: MIT
 *
 * Runs against a synthetic $HOME under /tmp so tests don't touch
 * the developer's real home.
 */

#include "nh_syncd.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static char g_home[256];

static void setup_home(void) {
    snprintf(g_home, sizeof g_home, "/tmp/nh_syncd_ignore_%d", (int)getpid());
    /* Best-effort cleanup from a prior aborted run. */
    char rmcmd[512];
    snprintf(rmcmd, sizeof rmcmd, "rm -rf %s", g_home);
    (void)system(rmcmd);
    assert(mkdir(g_home, 0700) == 0);
    /* No user ignore file. */
}

static void teardown_home(void) {
    char rmcmd[512];
    snprintf(rmcmd, sizeof rmcmd, "rm -rf %s", g_home);
    (void)system(rmcmd);
}

static void t_static_prefixes(void) {
    nh_syncd_ignore *ig = NULL;
    assert(nh_syncd_ignore_new(g_home, &ig) == 0);

    /* Control prefixes ALL block, including deep children. */
    assert(nh_syncd_ignore_check_path(ig, ".cache") == NH_SYNCD_IGNORE_CONTROL);
    assert(nh_syncd_ignore_check_path(ig, ".cache/mozilla/foo") == NH_SYNCD_IGNORE_CONTROL);
    assert(nh_syncd_ignore_check_path(ig, ".local/share/Trash/x") == NH_SYNCD_IGNORE_CONTROL);
    assert(nh_syncd_ignore_check_path(ig, ".local/state/nostr-homed/snapshot.json") == NH_SYNCD_IGNORE_CONTROL);
    assert(nh_syncd_ignore_check_path(ig, ".nostr-home-limited") == NH_SYNCD_IGNORE_CONTROL);
    /* NH_SYNCD_LOCAL_CACHE_REL constant. */
    char cache_child[128];
    snprintf(cache_child, sizeof cache_child, "%s/blobs/deadbeef", NH_SYNCD_LOCAL_CACHE_REL);
    assert(nh_syncd_ignore_check_path(ig, cache_child) == NH_SYNCD_IGNORE_CONTROL);

    /* Bare prefix without the trailing slash — must NOT match a
     * different name that only shares an initial substring. */
    assert(nh_syncd_ignore_check_path(ig, ".cacherefusable") == NH_SYNCD_IGNORE_PASS);
    assert(nh_syncd_ignore_check_path(ig, ".nostr-home-limited-not") == NH_SYNCD_IGNORE_PASS);

    nh_syncd_ignore_free(ig);
    printf("t_static_prefixes OK\n");
}

static void t_static_globs(void) {
    nh_syncd_ignore *ig = NULL;
    assert(nh_syncd_ignore_new(g_home, &ig) == 0);

    /* *.tmp anywhere. */
    assert(nh_syncd_ignore_check_path(ig, "foo.tmp") == NH_SYNCD_IGNORE_STATIC);
    assert(nh_syncd_ignore_check_path(ig, "docs/build/foo.tmp") == NH_SYNCD_IGNORE_STATIC);
    /* editor backup ~ */
    assert(nh_syncd_ignore_check_path(ig, "notes.txt~") == NH_SYNCD_IGNORE_STATIC);
    /* firefox lock */
    assert(nh_syncd_ignore_check_path(ig, ".mozilla/firefox/abc.profile/lock") == NH_SYNCD_IGNORE_STATIC);
    /* not-tmp */
    assert(nh_syncd_ignore_check_path(ig, "notes.tmpish") == NH_SYNCD_IGNORE_PASS);

    nh_syncd_ignore_free(ig);
    printf("t_static_globs OK\n");
}

static void t_user_file(void) {
    /* Write a user file with a couple of globs and a comment/blank. */
    char cfg_dir[300], cfg_file[512];
    snprintf(cfg_dir, sizeof cfg_dir, "%s/.config/nostr-homed", g_home);
    /* mkdir -p */
    char *p = strdup(cfg_dir);
    for (char *c = p + 1; *c; c++) {
        if (*c == '/') { *c = '\0'; mkdir(p, 0700); *c = '/'; }
    }
    mkdir(p, 0700);
    free(p);
    snprintf(cfg_file, sizeof cfg_file, "%s/ignore", cfg_dir);
    FILE *f = fopen(cfg_file, "w");
    assert(f);
    fprintf(f, "# comment line\n\n  build/*\n**/*.log\n");
    fclose(f);

    /* Point XDG_CONFIG_HOME at our fake so the matcher picks up the file
     * regardless of the real XDG_CONFIG_HOME on the developer's box. */
    char cfg_home[300];
    snprintf(cfg_home, sizeof cfg_home, "%s/.config", g_home);
    setenv("XDG_CONFIG_HOME", cfg_home, 1);

    nh_syncd_ignore *ig = NULL;
    assert(nh_syncd_ignore_new(g_home, &ig) == 0);

    assert(nh_syncd_ignore_check_path(ig, "build/anything") == NH_SYNCD_IGNORE_USER);
    assert(nh_syncd_ignore_check_path(ig, "src/foo.log") == NH_SYNCD_IGNORE_USER);
    assert(nh_syncd_ignore_check_path(ig, "src/foo.txt") == NH_SYNCD_IGNORE_PASS);

    nh_syncd_ignore_free(ig);
    unsetenv("XDG_CONFIG_HOME");
    printf("t_user_file OK\n");
}

static void t_xdev(void) {
    nh_syncd_ignore *ig = NULL;
    assert(nh_syncd_ignore_new(g_home, &ig) == 0);
    /* Force a specific home_dev via the test seam and pass a different one. */
    nh_syncd_ignore_set_home_dev(ig, 0x1111);
    assert(nh_syncd_ignore_check(ig, "mnt/usb/photos", 0x2222, 0x8000 /* S_IFREG */) == NH_SYNCD_IGNORE_XDEV);
    assert(nh_syncd_ignore_check(ig, "mnt/usb/photos", 0x1111, 0x8000) == NH_SYNCD_IGNORE_PASS);
    nh_syncd_ignore_free(ig);
    printf("t_xdev OK\n");
}

static void t_special_files(void) {
    nh_syncd_ignore *ig = NULL;
    assert(nh_syncd_ignore_new(g_home, &ig) == 0);
    nh_syncd_ignore_set_home_dev(ig, 0x1111);
    /* Socket (S_IFSOCK = 0140000). */
    assert(nh_syncd_ignore_check(ig, "run/foo.sock", 0x1111, 0140000) == NH_SYNCD_IGNORE_SPECIAL);
    /* FIFO. */
    assert(nh_syncd_ignore_check(ig, "run/pipe", 0x1111, 0010000) == NH_SYNCD_IGNORE_SPECIAL);
    /* Regular file passes. */
    assert(nh_syncd_ignore_check(ig, "run/regular", 0x1111, 0100644) == NH_SYNCD_IGNORE_PASS);
    nh_syncd_ignore_free(ig);
    printf("t_special_files OK\n");
}

int main(void) {
    setup_home();
    t_static_prefixes();
    t_static_globs();
    t_user_file();
    t_xdev();
    t_special_files();
    teardown_home();
    printf("test_syncd_ignore: OK\n");
    return 0;
}
