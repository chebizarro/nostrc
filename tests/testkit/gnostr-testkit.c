/**
 * gnostr-testkit.c — Shared test infrastructure implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gnostr-testkit.h"
#include <nostr-gobject-1.0/storage_ndb.h>
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
extern int storage_ndb_init(const char *db_dir, const char *opts_json, GError **error);
extern void storage_ndb_shutdown(void);
extern int storage_ndb_ingest_event_json(const char *json, const char *relay_opt);

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

    /* Default opts: small mapsize for tests (64 MB), skip validation for test events */
    const char *opts = opts_json ? opts_json :
        "{\"mapsize\": 67108864, \"ingester_threads\": 1, \"ingest_skip_validation\": 1}";

    int ret = storage_ndb_init(ndb->dir, opts, NULL);
    if (ret == 0) {
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

    (void)ndb;
    /* Use async ingestion - caller must call gn_test_ndb_wait_for_ingest() after batch */
    int ret = storage_ndb_ingest_event_json(json, NULL);
    return ret == 0;
}

void
gn_test_ndb_wait_for_ingest(void)
{
    /* NDB uses async ingester threads. Poll with exponential backoff up to 1 second.
     * This is more reliable than fixed delays as it adapts to system load. */
    const int max_attempts = 10;
    int delay_us = 10000; /* Start with 10ms */
    
    for (int i = 0; i < max_attempts; i++) {
        g_usleep(delay_us);
        /* Double delay each iteration: 10ms, 20ms, 40ms, 80ms, 160ms, 320ms... */
        delay_us = MIN(delay_us * 2, 200000); /* Cap at 200ms per iteration */
    }
    /* Total wait: ~10 + 20 + 40 + 80 + 160 + 200*4 = 1110ms */
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
        /* CRITICAL: Shutdown the global g_store so subsequent tests can init their own NDB.
         * Without this, storage_ndb_init() returns early and reuses the old instance. */
        storage_ndb_shutdown();
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

/* ════════════════════════════════════════════════════════════════════
 * Main-Thread NDB Violation Detection
 * ════════════════════════════════════════════════════════════════════ */

/* These are weak symbols — if storage_ndb was not compiled with
 * GNOSTR_TESTING, the functions resolve to no-ops at link time.
 * This lets the testkit work both with and without instrumentation. */

void storage_ndb_testing_mark_main_thread(void) __attribute__((weak));
void storage_ndb_testing_clear_main_thread(void) __attribute__((weak));
unsigned storage_ndb_testing_get_violation_count(void) __attribute__((weak));
void storage_ndb_testing_reset_violations(void) __attribute__((weak));
const char *storage_ndb_testing_get_violation_func(unsigned index) __attribute__((weak));

static gboolean gn_test_instrumentation_warned = FALSE;

static void
gn_test_warn_no_instrumentation(void)
{
    if (!gn_test_instrumentation_warned) {
        g_test_message("WARNING: NDB violation instrumentation not available. "
                       "Ensure nostr-gobject was compiled with -DGNOSTR_TESTING.");
        gn_test_instrumentation_warned = TRUE;
    }
}

void
gn_test_mark_main_thread(void)
{
    if (storage_ndb_testing_mark_main_thread)
        storage_ndb_testing_mark_main_thread();
    else
        gn_test_warn_no_instrumentation();
}

void
gn_test_clear_main_thread(void)
{
    if (storage_ndb_testing_clear_main_thread)
        storage_ndb_testing_clear_main_thread();
}

void
gn_test_reset_ndb_violations(void)
{
    if (storage_ndb_testing_reset_violations)
        storage_ndb_testing_reset_violations();
}

unsigned
gn_test_get_ndb_violation_count(void)
{
    if (storage_ndb_testing_get_violation_count)
        return storage_ndb_testing_get_violation_count();
    return 0;
}

void
gn_test_assert_no_ndb_violations(const char *context)
{
    unsigned count = gn_test_get_ndb_violation_count();
    if (count == 0) return;

    g_test_message("╔════════════════════════════════════════════════════╗");
    g_test_message("║ MAIN-THREAD NDB VIOLATIONS: %u %s", count,
                   context ? context : "");
    g_test_message("╠════════════════════════════════════════════════════╣");

    if (storage_ndb_testing_get_violation_func) {
        unsigned show = count < 20 ? count : 20;
        for (unsigned i = 0; i < show; i++) {
            const char *fn = storage_ndb_testing_get_violation_func(i);
            g_test_message("║  [%u] %s", i, fn ? fn : "(unknown)");
        }
        if (count > 20)
            g_test_message("║  ... and %u more", count - 20);
    }

    g_test_message("╚════════════════════════════════════════════════════╝");
    g_test_message("");
    g_test_message("FIX: Move NDB transactions to a GTask worker thread.");
    g_test_message("     Use g_task_run_in_thread() for storage_ndb queries,");
    g_test_message("     then marshal results back to the main thread via");
    g_test_message("     the GTask callback.");

    g_test_fail_printf("Expected zero main-thread NDB violations %s, got %u",
                       context ? context : "", count);
}

/* ════════════════════════════════════════════════════════════════════
 * Realistic Event Corpus Generation
 * ════════════════════════════════════════════════════════════════════ */

static const char *short_texts[] = {
    "gm ☀️",
    "this is a test note",
    "hello world from nostr!",
    "just setting up my nostr",
    "LFG 🚀",
    "testing 1 2 3",
    "nostr is the way",
    "bitcoin fixes this",
};

static const char *medium_templates[] = {
    "Just published a new article about #nostr development. "
    "Check it out: https://example.com/article/%d\n\n"
    "Key takeaways:\n- Decentralization matters\n- NIPs are evolving\n"
    "#bitcoin #freedom",

    "Interesting thread on the future of social media. "
    "The key insight is that protocol-level identity (NIP-05) "
    "combined with relay selection gives users real control. "
    "https://relay.example.com/thread/%d #nostr #decentralization",

    "Today I learned about NIP-57 zaps and how they work under the hood. "
    "The bolt11 invoice parsing is surprisingly elegant. "
    "Lightning payments + social media = unstoppable 🚀⚡ "
    "https://nostr-resources.com/%d",
};

static const char *long_template =
    "🧵 Thread on Nostr protocol internals (1/%d)\n\n"
    "Let me break down how event propagation works in the nostr network. "
    "When you publish an event, it gets sent to all your connected relays. "
    "Each relay validates the event signature using secp256k1.\n\n"
    "The relay then stores the event in its database (many use LMDB via nostrdb) "
    "and notifies any clients that have active subscriptions matching the event's "
    "kind, authors, or tags.\n\n"
    "This is fundamentally different from centralized platforms where a single "
    "server controls the entire message flow. In nostr, the client chooses which "
    "relays to publish to and read from.\n\n"
    "Key NIPs involved:\n"
    "- NIP-01: Basic protocol\n"
    "- NIP-02: Contact list\n"
    "- NIP-10: Thread markers\n"
    "- NIP-25: Reactions\n"
    "- NIP-57: Zaps\n\n"
    "The implications for censorship resistance are profound. No single entity "
    "can silence a user because they can always find new relays to publish to. "
    "The tradeoff is that content moderation becomes a client-side concern.\n\n"
    "#nostr #protocol #decentralization #bitcoin #freedom #opensource 🔑";

static const char *unicode_texts[] = {
    "测试中文内容 🇨🇳 これはテストです 🇯🇵 한국어 테스트 🇰🇷\n"
    "Mixed script: αβγδ Кириллица العربية\n"
    "Emoji storm: 🎉🎊🎈🎁🎂🎄🎃🎇🎆✨🎍🎋🎏🎎🎌🏮",

    "Zero-width: foo\u200Bbar\u200Cbaz\u200Dqux\uFEFFend\n"
    "Combining marks: e\u0301 a\u0308 n\u0303 o\u0302\n"
    "Surrogates: 𝕳𝖊𝖑𝖑𝖔 𝕹𝖔𝖘𝖙𝖗 🔐",
};

static const char *url_templates[] = {
    "Check out this image: https://image.nostr.build/%08x.jpg\n"
    "And this video: https://v.nostr.build/%08x.mp4\n"
    "Also: https://nitter.net/status/%d https://stacker.news/items/%d\n"
    "Source: https://github.com/nostr-protocol/nips/blob/master/01.md",

    "Media dump:\nhttps://cdn.example.com/photo_%d.png\n"
    "https://cdn.example.com/video_%d.webm\n"
    "https://cdn.example.com/audio_%d.mp3\n"
    "https://cdn.example.com/doc_%d.pdf\n"
    "#media #content",
};

char *
gn_test_make_realistic_event_json(int kind,
                                   GnTestContentStyle style,
                                   gint64 created_at)
{
    /* For MIXED style, pick a random sub-style */
    GnTestContentStyle actual = style;
    if (actual == GN_TEST_CONTENT_MIXED) {
        actual = (GnTestContentStyle)g_random_int_range(0, GN_TEST_CONTENT_MENTIONS);
    }

    g_autofree char *content = NULL;
    guint32 r = (guint32)g_random_int();

    switch (actual) {
    case GN_TEST_CONTENT_SHORT:
        content = g_strdup(short_texts[r % G_N_ELEMENTS(short_texts)]);
        break;
    case GN_TEST_CONTENT_MEDIUM:
        content = g_strdup_printf(
            medium_templates[r % G_N_ELEMENTS(medium_templates)], r);
        break;
    case GN_TEST_CONTENT_LONG:
        content = g_strdup_printf(long_template, r % 20 + 2);
        break;
    case GN_TEST_CONTENT_UNICODE:
        content = g_strdup(unicode_texts[r % G_N_ELEMENTS(unicode_texts)]);
        break;
    case GN_TEST_CONTENT_URLS:
        content = g_strdup_printf(
            url_templates[r % G_N_ELEMENTS(url_templates)], r, r + 1, r + 2, r + 3);
        break;
    case GN_TEST_CONTENT_MENTIONS: {
        char fake_npub[65] = {0};
        random_hex(fake_npub, 32);
        content = g_strdup_printf(
            "nostr:%s mentioned something interesting about "
            "#nostr development. The thread is worth reading.\n"
            "cc nostr:%s",
            fake_npub, fake_npub);
        break;
    }
    default:
        content = g_strdup("fallback content");
        break;
    }

    return gn_test_make_event_json(kind, content, created_at);
}

char *
gn_test_make_profile_event_json(const char *pubkey_hex,
                                 const char *name,
                                 const char *about,
                                 const char *picture_url,
                                 gint64 created_at)
{
    g_autofree char *escaped_name = g_strescape(name ? name : "", NULL);
    g_autofree char *escaped_about = g_strescape(about ? about : "", NULL);

    g_autofree char *content = NULL;
    if (picture_url) {
        g_autofree char *escaped_pic = g_strescape(picture_url, NULL);
        content = g_strdup_printf(
            "{\"name\":\"%s\",\"about\":\"%s\",\"picture\":\"%s\"}",
            escaped_name, escaped_about, escaped_pic);
    } else {
        content = g_strdup_printf(
            "{\"name\":\"%s\",\"about\":\"%s\"}",
            escaped_name, escaped_about);
    }

    return gn_test_make_event_json_with_pubkey(0, content, created_at, pubkey_hex);
}

GPtrArray *
gn_test_ingest_realistic_corpus(GnTestNdb *ndb, guint n_events, guint n_profiles)
{
    g_return_val_if_fail(ndb != NULL, NULL);

    GPtrArray *pubkeys = g_ptr_array_new_with_free_func(g_free);

    /* Generate unique pubkeys */
    guint n_unique = MAX(n_profiles, 1);
    for (guint i = 0; i < n_unique; i++) {
        char pk[65] = {0};
        random_hex(pk, 32);
        g_ptr_array_add(pubkeys, g_strdup(pk));
    }

    /* Ingest profiles first (so model readiness checks pass) */
    for (guint i = 0; i < n_unique && i < n_profiles; i++) {
        const char *pk = g_ptr_array_index(pubkeys, i);
        g_autofree char *name = g_strdup_printf("TestUser_%u", i);
        g_autofree char *about = g_strdup_printf("Test profile #%u for corpus", i);
        g_autofree char *pic_url = g_strdup_printf(
            "https://robohash.org/%s.png", pk);
        g_autofree char *json = gn_test_make_profile_event_json(
            pk, name, about, pic_url, 1700000000 + i);
        gn_test_ndb_ingest_json(ndb, json);
    }

    /* Ingest events with varied content styles */
    for (guint i = 0; i < n_events; i++) {
        GnTestContentStyle style = (GnTestContentStyle)(i % (GN_TEST_CONTENT_MIXED + 1));
        gint64 ts = 1700000000 - (gint64)i;

        /* Use explicit pubkey so profile readiness works */
        const char *pk = g_ptr_array_index(pubkeys, i % n_unique);

        /* Generate content based on style */
        g_autofree char *content = NULL;
        guint32 r = (guint32)g_random_int();
        switch (style) {
        case GN_TEST_CONTENT_SHORT:
            content = g_strdup(short_texts[r % G_N_ELEMENTS(short_texts)]);
            break;
        case GN_TEST_CONTENT_MEDIUM:
            content = g_strdup_printf(
                medium_templates[r % G_N_ELEMENTS(medium_templates)], r);
            break;
        case GN_TEST_CONTENT_LONG:
            content = g_strdup_printf(long_template, r % 20 + 2);
            break;
        case GN_TEST_CONTENT_UNICODE:
            content = g_strdup(unicode_texts[r % G_N_ELEMENTS(unicode_texts)]);
            break;
        case GN_TEST_CONTENT_URLS:
            content = g_strdup_printf(
                url_templates[r % G_N_ELEMENTS(url_templates)], r, r + 1, r + 2, r + 3);
            break;
        default:
            content = g_strdup_printf("Corpus note #%u with %s style", i,
                style == GN_TEST_CONTENT_MENTIONS ? "mentions" : "mixed");
            break;
        }

        g_autofree char *json = gn_test_make_event_json_with_pubkey(1, content, ts, pk);
        gn_test_ndb_ingest_json(ndb, json);
    }

    /* Wait for async ingestion to complete */
    gn_test_ndb_wait_for_ingest();

    return pubkeys;
}

/* ════════════════════════════════════════════════════════════════════
 * Heartbeat (main-loop stall detection)
 * ════════════════════════════════════════════════════════════════════ */

static gboolean
heartbeat_tick_cb(gpointer user_data)
{
    GnTestHeartbeat *hb = user_data;
    gint64 now = g_get_monotonic_time();

    if (hb->last_us > 0) {
        gint64 gap = now - hb->last_us;
        if (gap > hb->max_gap_us)
            hb->max_gap_us = gap;
        if (gap > hb->max_stall_us)
            hb->missed_count++;
    }

    hb->last_us = now;
    hb->count++;
    return G_SOURCE_CONTINUE;
}

void
gn_test_heartbeat_start(GnTestHeartbeat *hb, guint interval_ms, guint max_stall_ms)
{
    g_return_if_fail(hb != NULL);

    memset(hb, 0, sizeof(*hb));
    hb->interval_ms = interval_ms;
    hb->max_stall_us = (gint64)max_stall_ms * 1000;
    hb->source_id = g_timeout_add(interval_ms, heartbeat_tick_cb, hb);
}

void
gn_test_heartbeat_stop(GnTestHeartbeat *hb)
{
    g_return_if_fail(hb != NULL);

    if (hb->source_id > 0) {
        g_source_remove(hb->source_id);
        hb->source_id = 0;
    }

    g_test_message("Heartbeat summary: count=%u, missed=%u, max_gap=%.1fms, "
                   "threshold=%.1fms",
                   hb->count, hb->missed_count,
                   hb->max_gap_us / 1000.0,
                   hb->max_stall_us / 1000.0);
}
