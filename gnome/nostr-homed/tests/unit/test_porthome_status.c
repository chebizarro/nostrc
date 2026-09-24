/*
 * test_porthome_status.c — merge-file concurrent-writers unit test
 * (bead nostrc-h10m.1).
 *
 * SPDX-License-Identifier: MIT
 *
 * Fork()s N children writing distinct top-level keys concurrently.
 * The parent waits for all, then asserts the final file has every
 * key body intact. Also covers the corrupted-read → treated-as-empty
 * path.
 */

#define _GNU_SOURCE
#include "nh_porthome_status.h"

#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void write_all(const char *path, const char *body) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    size_t L = strlen(body);
    assert(write(fd, body, L) == (ssize_t)L);
    close(fd);
}

static void test_get_key(void) {
    const char *doc =
        "{\"schema\":1,"
         "\"syncd\":{\"state\":\"ready\",\"gen\":42},"
         "\"fuse\":{\"mounted\":true,\"gen\":5}}";
    char *out = NULL;
    int rc = nh_porthome_status_get_key(doc, strlen(doc), "syncd", &out);
    assert(rc == 0);
    assert(out);
    assert(strstr(out, "\"state\":\"ready\"") != NULL);
    assert(strstr(out, "\"gen\":42")         != NULL);
    free(out);

    rc = nh_porthome_status_get_key(doc, strlen(doc), "provisioner", &out);
    assert(rc == -ENOENT);

    printf("get_key OK\n");
}

static void test_corrupt_read(void) {
    char tmpl[] = "/tmp/nh_status_corrupt.XXXXXX";
    int fd = mkstemp(tmpl);
    assert(fd >= 0);
    close(fd);
    write_all(tmpl, "this is not json {{{ >>>");

    char *body = NULL; size_t body_len = 0;
    int rc = nh_porthome_status_read(tmpl, &body, &body_len);
    assert(rc == 0);
    assert(body);
    /* Treated as empty document seed. */
    assert(strstr(body, "\"schema\":1") != NULL);
    free(body);
    unlink(tmpl);
    printf("corrupt_read OK\n");
}

static void test_write_and_replace(void) {
    char tmpl[] = "/tmp/nh_status_rw.XXXXXX";
    int fd = mkstemp(tmpl);
    assert(fd >= 0);
    close(fd);
    unlink(tmpl);

    /* Missing file → create with the key. */
    int rc = nh_porthome_status_write_key(tmpl, "syncd",
        "{\"state\":\"ready\",\"gen\":1}");
    assert(rc == 0);

    char *body = NULL; size_t body_len = 0;
    rc = nh_porthome_status_read(tmpl, &body, &body_len);
    assert(rc == 0);
    assert(strstr(body, "\"syncd\":") != NULL);
    assert(strstr(body, "\"state\":\"ready\"") != NULL);
    free(body);

    /* Add fuse key. */
    rc = nh_porthome_status_write_key(tmpl, "fuse",
        "{\"mounted\":true,\"gen\":3}");
    assert(rc == 0);

    rc = nh_porthome_status_read(tmpl, &body, &body_len);
    assert(rc == 0);
    assert(strstr(body, "\"syncd\":") != NULL);
    assert(strstr(body, "\"fuse\":")  != NULL);
    free(body);

    /* Replace syncd key. */
    rc = nh_porthome_status_write_key(tmpl, "syncd",
        "{\"state\":\"partial\",\"gen\":9}");
    assert(rc == 0);

    rc = nh_porthome_status_read(tmpl, &body, &body_len);
    assert(rc == 0);
    assert(strstr(body, "\"state\":\"partial\"") != NULL);
    assert(strstr(body, "\"gen\":9")              != NULL);
    /* Old value gone. */
    assert(strstr(body, "\"gen\":1")              == NULL);
    /* fuse key still present. */
    assert(strstr(body, "\"fuse\":")              != NULL);
    free(body);

    unlink(tmpl);
    char lock[300]; snprintf(lock, sizeof lock, "%s.lock", tmpl);
    unlink(lock);
    printf("write_and_replace OK\n");
}

static void test_concurrent_writers(void) {
    char tmpl[] = "/tmp/nh_status_conc.XXXXXX";
    int fd = mkstemp(tmpl);
    assert(fd >= 0);
    close(fd);
    unlink(tmpl); /* We want write_key to create it fresh. */

    /* Seed with an initial document so all writers merge against it. */
    int rc = nh_porthome_status_write_key(tmpl, "schema_bootstrap",
                                          "\"init\"");
    assert(rc == 0);

    const char *keys[] = {
        "syncd", "fuse", "provisioner", "extra1", "extra2",
        "extra3", "extra4", "extra5",
    };
    size_t nk = sizeof keys / sizeof keys[0];
    pid_t pids[16];
    for (size_t i = 0; i < nk; ++i) {
        pid_t p = fork();
        assert(p >= 0);
        if (p == 0) {
            /* Child writes its key body 20 times to increase the odds
             * of an overlap on shared lockfile. */
            char body[128];
            for (int r = 0; r < 20; ++r) {
                snprintf(body, sizeof body,
                         "{\"iter\":%d,\"pid\":%d}",
                         r, (int)getpid());
                int rc2 = nh_porthome_status_write_key(tmpl, keys[i], body);
                if (rc2 != 0) _exit(11);
            }
            _exit(0);
        }
        pids[i] = p;
    }
    for (size_t i = 0; i < nk; ++i) {
        int st = 0;
        assert(waitpid(pids[i], &st, 0) == pids[i]);
        assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }

    /* Every key survives. */
    char *body = NULL; size_t body_len = 0;
    rc = nh_porthome_status_read(tmpl, &body, &body_len);
    assert(rc == 0);
    for (size_t i = 0; i < nk; ++i) {
        char k[64];
        snprintf(k, sizeof k, "\"%s\":", keys[i]);
        if (!strstr(body, k)) {
            fprintf(stderr, "missing key %s in body:\n%s\n", keys[i], body);
            abort();
        }
    }
    free(body);

    unlink(tmpl);
    char lock[300]; snprintf(lock, sizeof lock, "%s.lock", tmpl);
    unlink(lock);
    printf("concurrent_writers OK (%zu keys × 20 writes)\n", nk);
}

static void test_escape(void) {
    char buf[128];
    int n = nh_porthome_json_escape("a\"b\\c\n", buf, sizeof buf);
    assert(n > 0);
    assert(!strcmp(buf, "a\\\"b\\\\c\\n"));
    printf("escape OK\n");
}

int main(void) {
    test_get_key();
    test_corrupt_read();
    test_write_and_replace();
    test_concurrent_writers();
    test_escape();
    printf("test_porthome_status: all tests passed\n");
    return 0;
}
