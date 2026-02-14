/**
 * gnostr-testkit.c — Shared test infrastructure implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gnostr-testkit.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <glib/gstdio.h>

#ifdef __APPLE__
#include <mach/mach.h>
#endif

/* ════════════════════════════════════════════════════════════════════
 * Temporary NDB Instance
 * ════════════════════════════════════════════════════════════════════ */

struct _GnTestNdb {
    char *dir;
    gboolean initialized;
};

/* Forward declare storage_ndb functions — these come from nostr-gobject.
 * We declare them here to avoid pulling the full header, keeping
 * testkit's header dependency minimal. */
extern int storage_ndb_init(const char *db_dir, const char *opts_json);
extern void storage_ndb_shutdown(void);
extern int storage_ndb_ingest_event(const char *json, size_t json_len);

GnTestNdb *
gn_test_ndb_new(const char *opts_json)
{
    GnTestNdb *ndb = g_new0(GnTestNdb, 1);

    /* Create a unique temporary directory */
    char tmpl[] = "/tmp/gnostr-test-ndb-XXXXXX";
    char *dir = g_mkdtemp(tmpl);
    if (!dir) {
        g_warning("gn_test_ndb_new: failed to create temp dir");
        g_free(ndb);
        return NULL;
    }
    ndb->dir = g_strdup(dir);

    /* Default opts: small mapsize for tests (64 MB) */
    const char *opts = opts_json ? opts_json :
        "{\"mapsize\": 67108864, \"ingester_threads\": 1}";

    int ret = storage_ndb_init(ndb->dir, opts);
    if (ret != 0) {
        g_warning("gn_test_ndb_new: storage_ndb_init failed with %d", ret);
        g_free(ndb->dir);
        g_free(ndb);
        return NULL;
    }
    ndb->initialized = TRUE;

    return ndb;
}

const char *
gn_test_ndb_get_dir(GnTestNdb *ndb)
{
    g_return_val_if_fail(ndb != NULL, NULL);
    return ndb->dir;
}

gboolean
gn_test_ndb_ingest_json(GnTestNdb *ndb, const char *json)
{
    g_return_val_if_fail(ndb != NULL, FALSE);
    g_return_val_if_fail(json != NULL, FALSE);
    g_return_val_if_fail(ndb->initialized, FALSE);

    int ret = storage_ndb_ingest_event(json, strlen(json));
    return ret == 0;
}

/* Recursively remove a directory and its contents */
static void
remove_dir_recursive(const char *path)
{
    GDir *dir = g_dir_open(path, 0, NULL);
    if (!dir) return;

    const char *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        g_autofree char *child = g_build_filename(path, name, NULL);
        if (g_file_test(child, G_FILE_TEST_IS_DIR)) {
            remove_dir_recursive(child);
        } else {
            g_unlink(child);
        }
    }
    g_dir_close(dir);
    g_rmdir(path);
}

void
gn_test_ndb_free(GnTestNdb *ndb)
{
    if (!ndb) return;

    if (ndb->initialized) {
        storage_ndb_shutdown();
        ndb->initialized = FALSE;
    }

    if (ndb->dir) {
        remove_dir_recursive(ndb->dir);
        g_free(ndb->dir);
    }

    g_free(ndb);
}

/* ════════════════════════════════════════════════════════════════════
 * Event Fixture Generation
 * ════════════════════════════════════════════════════════════════════ */

/* Generate a random hex string of the given byte length (output is 2*n chars) */
static void
random_hex(char *buf, gsize n_bytes)
{
    for (gsize i = 0; i < n_bytes; i++) {
        guint8 byte = (guint8)g_random_int_range(0, 256);
        snprintf(buf + i * 2, 3, "%02x", byte);
    }
}

char *
gn_test_make_event_json(int kind, const char *content, gint64 created_at)
{
    char pubkey[65] = {0};
    random_hex(pubkey, 32);
    return gn_test_make_event_json_with_pubkey(kind, content, created_at, pubkey);
}

char *
gn_test_make_event_json_with_pubkey(int kind,
                                     const char *content,
                                     gint64 created_at,
                                     const char *pubkey_hex)
{
    char id[65] = {0};
    char sig[129] = {0};
    random_hex(id, 32);
    random_hex(sig, 64);

    /* Escape content for JSON (minimal: escape quotes and backslashes) */
    g_autofree char *escaped = g_strescape(content ? content : "", NULL);

    return g_strdup_printf(
        "{\"id\":\"%s\","
        "\"pubkey\":\"%s\","
        "\"created_at\":%" G_GINT64_FORMAT ","
        "\"kind\":%d,"
        "\"tags\":[],"
        "\"content\":\"%s\","
        "\"sig\":\"%s\"}",
        id, pubkey_hex, created_at, kind, escaped, sig);
}

GPtrArray *
gn_test_make_events_bulk(guint n, int kind, gint64 start_ts)
{
    GPtrArray *events = g_ptr_array_new_with_free_func(g_free);

    for (guint i = 0; i < n; i++) {
        g_autofree char *content = g_strdup_printf("Test event %u", i);
        char *json = gn_test_make_event_json(kind, content, start_ts + i);
        g_ptr_array_add(events, json);
    }

    return events;
}

/* ════════════════════════════════════════════════════════════════════
 * Main Loop Helpers
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    GnTestPredicate pred;
    gpointer user_data;
    gboolean satisfied;
    gboolean timed_out;
} LoopUntilData;

static gboolean
loop_until_timeout_cb(gpointer user_data)
{
    LoopUntilData *data = user_data;
    data->timed_out = TRUE;
    return G_SOURCE_REMOVE;
}

gboolean
gn_test_run_loop_until(GnTestPredicate pred,
                        gpointer user_data,
                        guint timeout_ms)
{
    g_return_val_if_fail(pred != NULL, FALSE);

    LoopUntilData data = {
        .pred = pred,
        .user_data = user_data,
        .satisfied = FALSE,
        .timed_out = FALSE,
    };

    /* Install a timeout to prevent infinite loops */
    guint timeout_id = g_timeout_add(timeout_ms, loop_until_timeout_cb, &data);

    GMainContext *ctx = g_main_context_default();

    while (!data.satisfied && !data.timed_out) {
        g_main_context_iteration(ctx, TRUE);
        data.satisfied = pred(user_data);
    }

    /* Remove the timeout if we satisfied early */
    if (!data.timed_out) {
        g_source_remove(timeout_id);
    }

    return data.satisfied;
}

void
gn_test_drain_main_loop(void)
{
    GMainContext *ctx = g_main_context_default();

    /* Iterate until no more pending dispatches.
     * Use a safety counter to prevent infinite loops. */
    int safety = 10000;
    while (g_main_context_pending(ctx) && safety-- > 0) {
        g_main_context_iteration(ctx, FALSE);
    }
}

/* ════════════════════════════════════════════════════════════════════
 * Memory / Resource Measurement
 * ════════════════════════════════════════════════════════════════════ */

gsize
gn_test_get_rss_bytes(void)
{
#ifdef __linux__
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;

    gsize rss_kb = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (g_str_has_prefix(line, "VmRSS:")) {
            /* Format: "VmRSS:    12345 kB" */
            if (sscanf(line, "VmRSS: %zu", &rss_kb) == 1) {
                break;
            }
        }
    }
    fclose(f);

    return rss_kb * 1024;  /* Convert kB to bytes */
#elif defined(__APPLE__)
    /* macOS: use mach_task_basic_info */
    struct mach_task_basic_info info;
    mach_msg_type_number_t size = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &size) == KERN_SUCCESS) {
        return info.resident_size;
    }
    return 0;
#else
    return 0;
#endif
}

double
gn_test_get_rss_mb(void)
{
    return (double)gn_test_get_rss_bytes() / (1024.0 * 1024.0);
}

/* ════════════════════════════════════════════════════════════════════
 * Object Lifecycle Helpers
 * ════════════════════════════════════════════════════════════════════ */

static void
weak_notify_cb(gpointer data, GObject *where_the_object_was G_GNUC_UNUSED)
{
    GnTestPointerWatch *watch = data;
    watch->finalized = TRUE;
}

GnTestPointerWatch *
gn_test_watch_object(GObject *obj, const char *label)
{
    g_return_val_if_fail(G_IS_OBJECT(obj), NULL);

    GnTestPointerWatch *watch = g_new0(GnTestPointerWatch, 1);
    watch->finalized = FALSE;
    watch->label = label;

    g_object_weak_ref(obj, weak_notify_cb, watch);
    return watch;
}

void
gn_test_assert_finalized(GnTestPointerWatch *watch)
{
    g_assert_nonnull(watch);
    if (!watch->finalized) {
        g_test_message("Expected object '%s' to be finalized, but it wasn't",
                       watch->label ? watch->label : "(unknown)");
        g_test_fail();
    }
}

void
gn_test_assert_not_finalized(GnTestPointerWatch *watch)
{
    g_assert_nonnull(watch);
    if (watch->finalized) {
        g_test_message("Expected object '%s' to NOT be finalized, but it was",
                       watch->label ? watch->label : "(unknown)");
        g_test_fail();
    }
}
