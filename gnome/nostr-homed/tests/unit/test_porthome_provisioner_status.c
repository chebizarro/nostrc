/*
 * test_porthome_provisioner_status.c — Phase 5 follow-up (bead nostrc-3o91).
 *
 * SPDX-License-Identifier: MIT
 *
 * Covers the provisioner-side "provisioner" key writer:
 *   1. State machine PREPARING → FETCHING (with two chunk ticks) →
 *      VERIFYING → PUBLISHING → DONE lands a valid "provisioner" body
 *      each step, with last_state / last_provisioned_ts / chunks_done
 *      surfaced in the final render.
 *   2. Peer keys ("syncd", "fuse") placed by other writers survive the
 *      provisioner's emits (atomic merge, concurrent-write safety).
 *   3. Error path sets state="error" + last_error_class.
 *   4. Category slug on the notification matches "provision" (throttle
 *      key = "start"|"done"|"error:<class>").
 *
 * The test uses the NH_PORTHOME_STATUS_DIR override so no real user
 * home is required; the writer's emit function skips the chown branch
 * because tests run non-root.
 */

#define _GNU_SOURCE
#include "auth_porthome_status.h"

#include "nh_porthome_notify.h"
#include "nh_porthome_status.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Compose the same path emit_pstat would land at when
 * NH_PORTHOME_STATUS_DIR=<root> is set. */
static void status_path_for(const char *root, char *out, size_t cap) {
    snprintf(out, cap, "%s/.local/state/nostr-homed/porthome-status.json",
             root);
}

static void seed_peer_keys(const char *path) {
    /* Seed syncd + fuse contributions from peer writers so we can
     * assert the provisioner emit preserves them. */
    assert(nh_porthome_status_write_key(
        path, "syncd",
        "{\"state\":\"idle\",\"last_push_gen\":9}") == 0);
    assert(nh_porthome_status_write_key(
        path, "fuse",
        "{\"mounted\":true,\"generation\":11}") == 0);
}

static void assert_field_present(const char *body, const char *needle) {
    if (!strstr(body, needle)) {
        fprintf(stderr, "expected `%s` in body:\n%s\n", needle, body);
        abort();
    }
}
static void assert_field_absent(const char *body, const char *needle) {
    if (strstr(body, needle)) {
        fprintf(stderr, "expected `%s` ABSENT from body:\n%s\n", needle, body);
        abort();
    }
}

static void test_state_machine(const char *root) {
    char path[1024];
    status_path_for(root, path, sizeof path);
    seed_peer_keys(path);

    nh_provisioner_status_writer *w = nh_provisioner_status_writer_new();
    assert(w != NULL);

    /* PREPARING */
    nh_provisioner_status_set_state(w, NH_PROV_ST_PREPARING);
    assert(nh_provisioner_status_emit(w, "/nonexistent", 0, 0, root) == 0);
    {
        char *body = NULL; size_t body_len = 0;
        assert(nh_porthome_status_read(path, &body, &body_len) == 0);
        assert_field_present(body, "\"provisioner\":");
        assert_field_present(body, "\"state\":\"preparing\"");
        /* Peer keys still there. */
        assert_field_present(body, "\"syncd\":");
        assert_field_present(body, "\"fuse\":");
        assert_field_present(body, "\"generation\":11");
        free(body);
    }

    /* FETCHING + two chunk ticks. */
    nh_provisioner_status_set_state(w, NH_PROV_ST_FETCHING);
    nh_provisioner_status_set_chunks(w, /*done=*/1, /*pending=*/4);
    assert(nh_provisioner_status_emit(w, "/nonexistent", 0, 0, root) == 0);
    nh_provisioner_status_set_chunks(w, /*done=*/3, /*pending=*/2);
    assert(nh_provisioner_status_emit(w, "/nonexistent", 0, 0, root) == 0);
    {
        char *body = NULL; size_t body_len = 0;
        assert(nh_porthome_status_read(path, &body, &body_len) == 0);
        assert_field_present(body, "\"state\":\"fetching\"");
        assert_field_present(body, "\"chunks_done\":3");
        assert_field_present(body, "\"chunks_pending\":2");
        /* Peer keys preserved through mid-stream emits. */
        assert_field_present(body, "\"syncd\":");
        assert_field_present(body, "\"fuse\":");
        free(body);
    }

    /* VERIFYING → PUBLISHING → DONE. Also record last_provisioned_ts. */
    nh_provisioner_status_set_state(w, NH_PROV_ST_VERIFYING);
    assert(nh_provisioner_status_emit(w, "/nonexistent", 0, 0, root) == 0);
    nh_provisioner_status_set_state(w, NH_PROV_ST_PUBLISHING);
    assert(nh_provisioner_status_emit(w, "/nonexistent", 0, 0, root) == 0);

    int64_t ts = (int64_t)time(NULL);
    nh_provisioner_status_set_state(w, NH_PROV_ST_DONE);
    nh_provisioner_status_set_last_provisioned_ts(w, ts);
    assert(nh_provisioner_status_emit(w, "/nonexistent", 0, 0, root) == 0);
    {
        char *body = NULL; size_t body_len = 0;
        assert(nh_porthome_status_read(path, &body, &body_len) == 0);
        assert_field_present(body, "\"state\":\"done\"");
        /* last_state carries the previous transition (PUBLISHING). */
        assert_field_present(body, "\"last_state\":\"publishing\"");
        char needle[64];
        snprintf(needle, sizeof needle, "\"last_provisioned_ts\":%lld",
                 (long long)ts);
        assert_field_present(body, needle);
        /* Peer keys still intact after five emits. */
        assert_field_present(body, "\"syncd\":");
        assert_field_present(body, "\"fuse\":");
        free(body);
    }

    nh_provisioner_status_writer_free(w);
    (void)unlink(path);
    printf("state_machine OK\n");
}

static void test_error_path(const char *root) {
    char path[1024];
    status_path_for(root, path, sizeof path);
    (void)unlink(path);
    seed_peer_keys(path);

    nh_provisioner_status_writer *w = nh_provisioner_status_writer_new();
    assert(w != NULL);

    nh_provisioner_status_set_state(w, NH_PROV_ST_PREPARING);
    assert(nh_provisioner_status_emit(w, "/nonexistent", 0, 0, root) == 0);
    /* Recoverable network failure. */
    nh_provisioner_status_set_state(w, NH_PROV_ST_ERROR);
    nh_provisioner_status_set_last_error_class(w, "network");
    assert(nh_provisioner_status_emit(w, "/nonexistent", 0, 0, root) == 0);

    char *body = NULL; size_t body_len = 0;
    assert(nh_porthome_status_read(path, &body, &body_len) == 0);
    assert_field_present(body, "\"state\":\"error\"");
    assert_field_present(body, "\"last_error_class\":\"network\"");
    assert_field_present(body, "\"last_state\":\"preparing\"");
    /* Peer keys survive an error emit as well. */
    assert_field_present(body, "\"syncd\":");
    assert_field_present(body, "\"fuse\":");
    free(body);

    nh_provisioner_status_writer_free(w);
    (void)unlink(path);
    printf("error_path OK\n");
}

/* Confirm the initial state has no residual "last_error_class" content
 * once we set it to "" — clearing paths matter for a rerun. */
static void test_clear_error_class(const char *root) {
    char path[1024];
    status_path_for(root, path, sizeof path);
    (void)unlink(path);

    nh_provisioner_status_writer *w = nh_provisioner_status_writer_new();
    assert(w != NULL);
    nh_provisioner_status_set_state(w, NH_PROV_ST_ERROR);
    nh_provisioner_status_set_last_error_class(w, "sandbox");
    assert(nh_provisioner_status_emit(w, "/nonexistent", 0, 0, root) == 0);
    {
        char *body = NULL; size_t body_len = 0;
        assert(nh_porthome_status_read(path, &body, &body_len) == 0);
        assert_field_present(body, "\"last_error_class\":\"sandbox\"");
        free(body);
    }
    /* Successor run: clear + go to done. */
    nh_provisioner_status_set_last_error_class(w, "");
    nh_provisioner_status_set_state(w, NH_PROV_ST_DONE);
    assert(nh_provisioner_status_emit(w, "/nonexistent", 0, 0, root) == 0);
    {
        char *body = NULL; size_t body_len = 0;
        assert(nh_porthome_status_read(path, &body, &body_len) == 0);
        assert_field_absent(body, "\"last_error_class\":\"sandbox\"");
        assert_field_present(body, "\"last_error_class\":\"\"");
        assert_field_present(body, "\"state\":\"done\"");
        free(body);
    }
    nh_provisioner_status_writer_free(w);
    (void)unlink(path);
    printf("clear_error_class OK\n");
}

/* Confirm the category slug for PROVISION notifications matches the
 * documented "provision" bucket. This is the shared throttle key for
 * "start"/"done"/"error:<class>" events posted from the broker. */
static void test_category_slug(void) {
    const char *slug = nh_porthome_notify_category_slug(NH_NOTIFY_CAT_PROVISION);
    assert(slug != NULL);
    assert(strcmp(slug, "provision") == 0);
    printf("category_slug OK\n");
}

int main(void) {
    /* Use a scratch dir under /tmp for the fake $HOME. The writer path
     * resolver produces <root>/.local/state/nostr-homed/porthome-status.json
     * — we let its own mkdir_p create the intermediate dirs. */
    char root[] = "/tmp/nh_prov_status.XXXXXX";
    assert(mkdtemp(root) != NULL);

    test_state_machine(root);
    test_error_path(root);
    test_clear_error_class(root);
    test_category_slug();

    /* Best-effort cleanup — the child dirs may still hold the status
     * file's leftovers; rm -rf equivalent kept minimal (test harness
     * runs on a tmpfs anyway). */
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    (void)system(cmd);
    printf("test_porthome_provisioner_status: all tests passed\n");
    return 0;
}
