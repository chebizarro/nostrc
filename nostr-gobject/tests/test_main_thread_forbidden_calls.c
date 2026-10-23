/**
 * test_main_thread_forbidden_calls.c — Main-thread blocking detection
 *
 * Verifies that certain heavy operations (NDB queries, sleeps) are not
 * called from the main GLib thread context. This prevents UI stalling.
 *
 * Strategy:
 * - Records the main thread at test startup
 * - Calls potentially-blocking storage_ndb functions from the main thread
 * - Verifies that the operations complete within acceptable time budgets
 * - Uses timing to detect if any call blocks for too long
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib.h>
#include <time.h>
#ifdef __APPLE__
#include <libproc.h>
#include <mach/mach.h>
#include <poll.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stddef.h>
#include <sys/proc_info.h>
#include <sys/resource.h>
#include <unistd.h>
#endif
#include "gnostr-testkit.h"
#include <nostr-gobject-1.0/storage_ndb.h>
#include <nostr-gobject-1.0/nostr_store.h>

static GnTestNdb *test_ndb = NULL;
static GThread *main_thread = NULL;

/* Maximum acceptable time for a main-thread-called NDB operation (ms) */
#define MAX_MAIN_THREAD_OP_MS 50

static void
setup(void)
{
    main_thread = g_thread_self();
    test_ndb = gn_test_ndb_new(NULL);
    g_assert_nonnull(test_ndb);
}

static void
teardown(void)
{
    gn_test_ndb_free(test_ndb);
    test_ndb = NULL;
}

/* ── Test: Store operations should complete within time budget ─────── */
static void
test_store_query_within_budget(void)
{
    setup();

    g_autoptr(GNostrNdbStore) store = gnostr_ndb_store_new();

    /* Ingest some test events first */
    g_autoptr(GPtrArray) events = gn_test_make_events_bulk(100, 1, 1700000000);
    for (guint i = 0; i < events->len; i++) {
        gn_test_ndb_ingest_json(test_ndb, g_ptr_array_index(events, i));
    }

    /* Allow ingestion to complete */
    g_usleep(100000); /* 100ms for ingestion worker */

    /* Time the query operation on the main thread */
    gint64 start = g_get_monotonic_time();

    GError *error = NULL;
    char *json = gnostr_store_get_note_by_id(
        GNOSTR_STORE(store),
        "0000000000000000000000000000000000000000000000000000000000000000",
        &error);
    g_free(json);
    g_clear_error(&error);

    gint64 elapsed_us = g_get_monotonic_time() - start;
    double elapsed_ms = elapsed_us / 1000.0;

    g_test_message("get_note_by_id took %.2f ms (budget: %d ms)",
                   elapsed_ms, MAX_MAIN_THREAD_OP_MS);

    g_assert_cmpfloat(elapsed_ms, <, MAX_MAIN_THREAD_OP_MS);

    teardown();
}

/* ── Test: Subscribe/poll cycle within budget ─────────────────────── */
static void
test_subscribe_poll_within_budget(void)
{
    setup();

    g_autoptr(GNostrNdbStore) store = gnostr_ndb_store_new();

    gint64 start = g_get_monotonic_time();

    guint64 sub_id = gnostr_store_subscribe(GNOSTR_STORE(store),
                                             "{\"kinds\":[1],\"limit\":50}");
    g_assert_cmpuint(sub_id, >, 0);

    guint64 keys[50];
    gnostr_store_poll_notes(GNOSTR_STORE(store), sub_id, keys, 50);

    gnostr_store_unsubscribe(GNOSTR_STORE(store), sub_id);

    gint64 elapsed_us = g_get_monotonic_time() - start;
    double elapsed_ms = elapsed_us / 1000.0;

    g_test_message("Subscribe+poll+unsubscribe took %.2f ms (budget: %d ms)",
                   elapsed_ms, MAX_MAIN_THREAD_OP_MS);

    g_assert_cmpfloat(elapsed_ms, <, MAX_MAIN_THREAD_OP_MS);

    teardown();
}

/* ── Test: Batch operations within budget ─────────────────────────── */
static void
test_batch_operations_within_budget(void)
{
    setup();

    g_autoptr(GNostrNdbStore) store = gnostr_ndb_store_new();

    const char *ids[] = {
        "0000000000000000000000000000000000000000000000000000000000000001",
        "0000000000000000000000000000000000000000000000000000000000000002",
        "0000000000000000000000000000000000000000000000000000000000000003",
        "0000000000000000000000000000000000000000000000000000000000000004",
        "0000000000000000000000000000000000000000000000000000000000000005",
        NULL
    };

    gint64 start = g_get_monotonic_time();

    GHashTable *reactions = gnostr_store_count_reactions_batch(
        GNOSTR_STORE(store), ids, 5);
    if (reactions) g_hash_table_unref(reactions);

    GHashTable *zaps = gnostr_store_get_zap_stats_batch(
        GNOSTR_STORE(store), ids, 5);
    if (zaps) g_hash_table_unref(zaps);

    gint64 elapsed_us = g_get_monotonic_time() - start;
    double elapsed_ms = elapsed_us / 1000.0;

    g_test_message("Batch reactions+zaps took %.2f ms (budget: %d ms)",
                   elapsed_ms, MAX_MAIN_THREAD_OP_MS);

    g_assert_cmpfloat(elapsed_ms, <, MAX_MAIN_THREAD_OP_MS);

    teardown();
}

/* ── Test: Heartbeat is not starved during operations ─────────────── */

#define HEARTBEAT_CYCLES 50
#define HEARTBEAT_GAP_US (MAX_MAIN_THREAD_OP_MS * G_TIME_SPAN_MILLISECOND)

typedef void (*HeartbeatWork)(gpointer data, guint cycle);

typedef struct {
    guint heartbeat_count;
    guint missed_count;
    gint64 last_beat_us;
    gint64 max_gap_us;
    guint work_count;
    gint64 work_us[HEARTBEAT_CYCLES];
    gboolean missed_after_cycle[HEARTBEAT_CYCLES];
    guint interval_count;
    gint64 first_gap_us;
    gint64 final_gap_us;
    gboolean burst_complete;
    /* Diagnostic attribution only (nostrc-0cfsg.11): the subset of missed_count
     * observed after the store burst, while only the idle main loop runs. It
     * never changes missed_count or the pass/fail policy. */
    guint post_burst_missed_count;
    gint64 post_burst_max_gap_us;
    gint64 post_burst_wall_us;
    gint64 post_burst_main_cpu_us;
    HeartbeatWork work;
    gpointer work_data;
    GMainLoop *loop;
} HeartbeatData;

static void
heartbeat_record_at(HeartbeatData *hb, gint64 now)
{
    gint64 gap = now - hb->last_beat_us;
    hb->max_gap_us = MAX(hb->max_gap_us, gap);
    /* Preserve the original strict >50ms boundary and two-gap allowance. */
    if (gap > HEARTBEAT_GAP_US) {
        hb->missed_count++;
        if (hb->burst_complete)
            hb->post_burst_missed_count++;
        else if (hb->work_count > 0)
            hb->missed_after_cycle[hb->work_count - 1] = TRUE;
    }
    if (hb->burst_complete)
        hb->post_burst_max_gap_us = MAX(hb->post_burst_max_gap_us, gap);
    if (hb->interval_count++ == 0)
        hb->first_gap_us = gap;
    hb->last_beat_us = now;
}

static void
heartbeat_do_work(HeartbeatData *hb)
{
    g_assert_true(g_thread_self() == main_thread);
    gint64 start = g_get_monotonic_time();
    hb->work(hb->work_data, hb->work_count);
    hb->work_us[hb->work_count++] = g_get_monotonic_time() - start;
}

static gboolean
heartbeat_cb(gpointer user_data)
{
    HeartbeatData *hb = user_data;
    g_assert_true(g_thread_self() == main_thread);
    heartbeat_record_at(hb, g_get_monotonic_time());
    hb->heartbeat_count++;
    if (hb->burst_complete && hb->heartbeat_count > 5) {
        g_main_loop_quit(hb->loop);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}


/* ── Record-only scheduling evidence (nostrc-0cfsg.11.1) ──────────────
 *
 * Observes, never asserts, and never changes the gating workload:
 *  - always (nonintrusive: a few syscalls outside the timed bursts): the
 *    Darwin background/QoS state this process inherited, and main-thread CPU
 *    versus wall time over the post-burst idle phase;
 *  - ONLY when NOSTRC_HEARTBEAT_CONTROL=1 (set explicitly by
 *    heartbeat_sched/heartbeat-sched-probe.sh, never by CTest): a no-work
 *    control sleeper thread. Its effect on wake latency is not proven
 *    negligible, so default CTest never starts it and keeps the original
 *    single-thread workload and timing. A late control sleeper alongside near-zero
 *    main-thread CPU points at process/host wake latency rather than store
 *    code. Nothing here feeds missed_count or any assertion.
 * The metric-validity case below is registered only when
 * NOSTRC_HEARTBEAT_DIAGNOSTICS=1, so the gating registration stays the
 * original 8 cases. */

static gboolean
env_opt_in(const char *name)
{
    return g_strcmp0(g_getenv(name), "1") == 0;
}

#define CONTROL_INTERVAL_US (10 * G_TIME_SPAN_MILLISECOND)

/* Opt-in poll-function counterfactual (nostrc-0cfsg.11.1), set ONLY by
 * heartbeat_sched/heartbeat-sched-probe.sh. GLib builds g_poll() on Darwin
 * from select() (BROKEN_POLL), and XNU gives select() timeouts latency-QoS
 * leeway: under an inherited UTILITY clamp (latency tier 3) a 10 ms timeout
 * may legally fire about 40 ms late. poll(2) takes the kqueue path without
 * that leeway. Comparing the two on the same host separates OS timer leeway
 * from the remaining wake latency. Default CTest never sets it. */
#ifdef __APPLE__
G_STATIC_ASSERT(sizeof(GPollFD) == sizeof(struct pollfd));
G_STATIC_ASSERT(offsetof(GPollFD, fd) == offsetof(struct pollfd, fd));
G_STATIC_ASSERT(offsetof(GPollFD, events) == offsetof(struct pollfd, events));
G_STATIC_ASSERT(offsetof(GPollFD, revents) == offsetof(struct pollfd, revents));

static gint
heartbeat_posix_poll(GPollFD *fds, guint nfds, gint timeout)
{
    return poll((struct pollfd *)fds, nfds, timeout);
}
#endif

static const char *
heartbeat_poll_func_begin(GMainContext *ctx, GPollFunc *saved)
{
    *saved = NULL;
#ifdef __APPLE__
    if (g_strcmp0(g_getenv("NOSTRC_HEARTBEAT_POLL_FUNC"), "poll") == 0) {
        *saved = g_main_context_get_poll_func(ctx);
        g_main_context_set_poll_func(ctx, heartbeat_posix_poll);
        return "poll(2)";
    }
#else
    (void)ctx;
#endif
    return "glib-default";
}

static void
heartbeat_poll_func_end(GMainContext *ctx, GPollFunc saved)
{
    if (saved)
        g_main_context_set_poll_func(ctx, saved);
}

typedef struct {
    gint stop;
    guint wakes;            /* written by the control thread; read after join */
    gint64 max_late_us;
} SchedControl;

/* Calling-thread CPU time in microseconds, or -1 if unavailable (including
 * platforms without CLOCK_THREAD_CPUTIME_ID; the gating test never depends on it). On Darwin this
 * uses thread_info(): clock_gettime(CLOCK_THREAD_CPUTIME_ID) returned
 * wall-clock epoch time in this test binary on macOS 27, so it is not used. */
static gint64
thread_cpu_us(void)
{
#ifdef __APPLE__
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    mach_port_t self = mach_thread_self();
    kern_return_t kr = thread_info(self, THREAD_BASIC_INFO, (thread_info_t)&info, &count);
    mach_port_deallocate(mach_task_self(), self);
    if (kr != KERN_SUCCESS)
        return -1;
    return ((gint64)info.user_time.seconds + info.system_time.seconds) * G_USEC_PER_SEC +
           info.user_time.microseconds + info.system_time.microseconds;
#elif !defined(G_OS_WIN32) && defined(CLOCK_THREAD_CPUTIME_ID)
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return -1;
    return (gint64)ts.tv_sec * G_USEC_PER_SEC + ts.tv_nsec / 1000;
#else
    return -1; /* unavailable on this platform; record-only output prints -1 */
#endif
}

static gpointer
sched_control_thread(gpointer data)
{
    SchedControl *control = data;
    while (!g_atomic_int_get(&control->stop)) {
        gint64 start = g_get_monotonic_time();
        g_usleep(CONTROL_INTERVAL_US);
        gint64 late = g_get_monotonic_time() - start - CONTROL_INTERVAL_US;
        control->wakes++;
        control->max_late_us = MAX(control->max_late_us, late);
    }
    return NULL;
}

static void
report_sched_env(const HeartbeatData *hb, const SchedControl *control,
                 const char *poll_func)
{
#ifdef __APPLE__
    qos_class_t qos = QOS_CLASS_UNSPECIFIED;
    int relprio = 0;
    pthread_get_qos_class_np(pthread_self(), &qos, &relprio);
    g_test_message("SchedEnv: darwin_bg_process=%d darwin_bg_thread=%d qos=0x%x",
                   getpriority(PRIO_DARWIN_PROCESS, 0),
                   getpriority(PRIO_DARWIN_THREAD, 0), (unsigned)qos);
    /* The requested QoS above does not show a clamp; the scheduler priority
     * does (e.g. 20 under a UTILITY clamp, 31/47 otherwise). */
    struct proc_threadinfo ti;
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    if (proc_pidinfo(getpid(), PROC_PIDTHREADID64INFO, tid, &ti, sizeof ti) == (int)sizeof ti)
        g_test_message("SchedEnv: main_thread_sched_pri cur=%d base=%d max=%d",
                       ti.pth_curpri, ti.pth_priority, ti.pth_maxpriority);
    else
        g_test_message("SchedEnv: main_thread_sched_pri unavailable");
#else
    g_test_message("SchedEnv: darwin_bg=n/a");
#endif
    g_test_message("SchedEnv: poll_func=%s", poll_func);
    g_test_message("SchedEnv: post_burst_wall=%.1f ms main_thread_cpu=%.3f ms",
                   hb->post_burst_wall_us / 1000.0,
                   hb->post_burst_main_cpu_us / 1000.0);
    if (control)
        g_test_message("SchedEnv: control_thread=on control_wakes=%u control_max_late=%.1f ms",
                       control->wakes, control->max_late_us / 1000.0);
    else
        g_test_message("SchedEnv: control_thread=off");
}

static void
run_heartbeat(HeartbeatWork work, gpointer data)
{
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    GPollFunc saved_poll = NULL;
    const char *poll_func = heartbeat_poll_func_begin(g_main_context_default(), &saved_poll);
    HeartbeatData hb = {.work = work, .work_data = data, .loop = loop};
    guint hb_id = g_timeout_add(10, heartbeat_cb, &hb);

    /* Preserve the original burst: never wait for timer readiness between
     * operations. Anchor before cycle 1 so its first stall is visible. */
    hb.last_beat_us = g_get_monotonic_time();
    for (guint i = 0; i < HEARTBEAT_CYCLES; i++) {
        heartbeat_do_work(&hb);
        g_main_context_iteration(g_main_context_default(), FALSE);
    }
    /* Close the last interval even if the final nonblocking iteration did
     * not dispatch the timer. This sample is NOT counted as a heartbeat. */
    gint64 end = g_get_monotonic_time();
    hb.final_gap_us = end - hb.last_beat_us;
    heartbeat_record_at(&hb, end);
    hb.burst_complete = TRUE;
    SchedControl control = {0};
    gboolean control_ran = FALSE;
    if (hb.heartbeat_count > 5) {
        g_source_remove(hb_id);
    } else {
        GThread *control_thread = NULL;
        if (env_opt_in("NOSTRC_HEARTBEAT_CONTROL")) {
            control_thread = g_thread_new("hb-control", sched_control_thread, &control);
            control_ran = TRUE;
        }
        gint64 wall_start = g_get_monotonic_time();
        gint64 cpu_start = thread_cpu_us();
        g_main_loop_run(loop); /* Post-burst liveness only; no work pacing. */
        hb.post_burst_wall_us = g_get_monotonic_time() - wall_start;
        gint64 cpu_end = thread_cpu_us();
        hb.post_burst_main_cpu_us = (cpu_start >= 0 && cpu_end >= 0) ? cpu_end - cpu_start : -1;
        if (control_thread) {
            g_atomic_int_set(&control.stop, 1);
            g_thread_join(control_thread);
        }
    }

    g_test_message("Heartbeat: count=%u, missed=%u, max_gap=%.1f ms, cycles=%u",
                   hb.heartbeat_count, hb.missed_count,
                   hb.max_gap_us / 1000.0, hb.work_count);
    g_test_message("Initial interval=%.3f ms, explicit final interval=%.3f ms",
                   hb.first_gap_us / 1000.0, hb.final_gap_us / 1000.0);
    g_test_message("Attribution: burst_missed=%u post_burst_missed=%u post_burst_max_gap=%.1f ms",
                   hb.missed_count - hb.post_burst_missed_count,
                   hb.post_burst_missed_count,
                   hb.post_burst_max_gap_us / 1000.0);
    heartbeat_poll_func_end(g_main_context_default(), saved_poll);
    report_sched_env(&hb, control_ran ? &control : NULL, poll_func);
    for (guint i = 0; i < hb.work_count; i++) {
        g_test_message("cycle=%u operation=%.3f ms%s",
                       i + 1, hb.work_us[i] / 1000.0,
                       hb.missed_after_cycle[i] ? " MISSED after cycle" : "");
    }
    if (g_test_subprocess()) {
        /* GLib suppresses g_test_message() in trapped children. Keep the
         * negative control's measured initial/final evidence on stderr. */
        g_printerr("blocking control: first_missed=%u second_missed=%u third_missed=%u missed=%u cycles=%u\n",
                   hb.missed_after_cycle[0], hb.missed_after_cycle[1],
                   hb.missed_after_cycle[2], hb.missed_count, hb.work_count);
        g_printerr("final control: cycle48_missed=%u cycle49_missed=%u cycle50_missed=%u\n",
                   hb.missed_after_cycle[47], hb.missed_after_cycle[48],
                   hb.missed_after_cycle[49]);
    }
    g_assert_cmpuint(hb.work_count, ==, HEARTBEAT_CYCLES);
    g_assert_cmpuint(hb.heartbeat_count, >, 5);
    g_assert_cmpuint(hb.missed_count, <=, 2);
}

static void
store_cycle(gpointer data, guint cycle G_GNUC_UNUSED)
{
    GNostrStore *store = data;
    guint64 sub_id = gnostr_store_subscribe(store, "{\"kinds\":[1],\"limit\":10}");
    g_assert_cmpuint(sub_id, >, 0);
    guint64 keys[10];
    gnostr_store_poll_notes(store, sub_id, keys, 10);
    gnostr_store_unsubscribe(store, sub_id);
}

static void
test_heartbeat_not_starved(void)
{
    setup();
    g_autoptr(GNostrNdbStore) store = gnostr_ndb_store_new();
    run_heartbeat(store_cycle, store);
    teardown();
}

static void
blocking_cycle(gpointer data G_GNUC_UNUSED, guint cycle)
{
    if (cycle >= 3)
        return;
    /* Deliberately monopolize the main thread; no sleep/readiness surrogate.
     * Include cycle 1 to prove that the formerly blind initial delay counts. */
    gint64 deadline = g_get_monotonic_time() + HEARTBEAT_GAP_US + 10000;
    while (g_get_monotonic_time() < deadline) { }
}

static void
test_heartbeat_detects_blocking(void)
{
    if (g_test_subprocess()) {
        main_thread = g_thread_self();
        run_heartbeat(blocking_cycle, NULL);
        return;
    }
    g_test_trap_subprocess(NULL, 5 * G_TIME_SPAN_SECOND, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*blocking control: first_missed=1 second_missed=1 third_missed=1 missed=* cycles=50*");
    g_test_trap_assert_stderr("*assertion failed (hb.missed_count <= 2)*");
}

static void
final_blocking_cycle(gpointer data, guint cycle)
{
    if (cycle >= HEARTBEAT_CYCLES - 3)
        blocking_cycle(data, 0);
}

static void
test_heartbeat_detects_final_blocking(void)
{
    if (g_test_subprocess()) {
        main_thread = g_thread_self();
        run_heartbeat(final_blocking_cycle, NULL);
        return;
    }
    g_test_trap_subprocess(NULL, 5 * G_TIME_SPAN_SECOND, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*final control: cycle48_missed=1 cycle49_missed=1 cycle50_missed=1*");
    g_test_trap_assert_stderr("*assertion failed (hb.missed_count <= 2)*");
}

typedef struct {
    gint64 previous_end_us;
    guint pressured;
} BurstControl;

static void
burst_sensitive_cycle(gpointer data, guint cycle G_GNUC_UNUSED)
{
    BurstControl *control = data;
    if (control->previous_end_us &&
        g_get_monotonic_time() - control->previous_end_us < 5000) {
        if (++control->pressured == 3)
            g_printerr("burst control: pressured=3\n");
        blocking_cycle(NULL, 0);
    }
    gint64 end = g_get_monotonic_time() + 1000;
    while (g_get_monotonic_time() < end) { }
    control->previous_end_us = g_get_monotonic_time();
}

static void
test_heartbeat_detects_burst_pressure(void)
{
    if (g_test_subprocess()) {
        main_thread = g_thread_self();
        BurstControl control = {0};
        run_heartbeat(burst_sensitive_cycle, &control);
        return;
    }
    g_test_trap_subprocess(NULL, 10 * G_TIME_SPAN_SECOND, G_TEST_SUBPROCESS_DEFAULT);
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*burst control: pressured=3*");
    g_test_trap_assert_stderr("*assertion failed (hb.missed_count <= 2)*");
}

/* Locks the validity of the record-only CPU metric: a sleeping thread must
 * report far less CPU than wall time. (clock_gettime(CLOCK_THREAD_CPUTIME_ID)
 * failed this on macOS 27 by returning wall-clock time.) Observation only; it
 * does not touch the heartbeat policy. */
static void
test_sched_env_thread_cpu_excludes_sleep(void)
{
    gint64 cpu_start = thread_cpu_us();
    if (cpu_start < 0) {
        g_test_skip("thread CPU time unavailable on this platform");
        return;
    }
    gint64 wall_start = g_get_monotonic_time();
    g_usleep(30 * G_TIME_SPAN_MILLISECOND);
    gint64 cpu_end = thread_cpu_us();
    gint64 wall = g_get_monotonic_time() - wall_start;
    g_assert_cmpint(cpu_start, >=, 0);
    g_assert_cmpint(cpu_end, >=, 0);
    gint64 cpu = cpu_end - cpu_start;
    g_assert_cmpint(wall, >=, 30 * G_TIME_SPAN_MILLISECOND);
    g_assert_cmpint(cpu, <, wall / 2);
}

static void
test_heartbeat_gap_boundary(void)
{
    HeartbeatData hb = {.last_beat_us = 1000000};
    heartbeat_record_at(&hb, 1500000); /* Initial 500ms must not be invisible. */
    g_assert_cmpuint(hb.missed_count, ==, 1);
    heartbeat_record_at(&hb, 1550000); /* Exactly 50ms is not a missed beat. */
    g_assert_cmpuint(hb.missed_count, ==, 1);
    heartbeat_record_at(&hb, 1600001);
    g_assert_cmpuint(hb.missed_count, ==, 2);
    heartbeat_record_at(&hb, 1650002);
    g_assert_cmpuint(hb.missed_count, ==, 3);
    g_assert_cmpuint(hb.post_burst_missed_count, ==, 0);

    /* Attribution is diagnostic only: a post-burst gap still counts. */
    hb.burst_complete = TRUE;
    heartbeat_record_at(&hb, 1650002 + 500000);
    g_assert_cmpuint(hb.missed_count, ==, 4);
    g_assert_cmpuint(hb.post_burst_missed_count, ==, 1);
    g_assert_cmpint(hb.post_burst_max_gap_us, ==, 500000);
    heartbeat_record_at(&hb, 1650002 + 500000 + 50000); /* exactly 50ms */
    g_assert_cmpuint(hb.missed_count, ==, 4);
    g_assert_cmpuint(hb.post_burst_missed_count, ==, 1);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/nostr-gobject/latency/store-query-within-budget",
                    test_store_query_within_budget);
    g_test_add_func("/nostr-gobject/latency/subscribe-poll-within-budget",
                    test_subscribe_poll_within_budget);
    g_test_add_func("/nostr-gobject/latency/batch-operations-within-budget",
                    test_batch_operations_within_budget);
    g_test_add_func("/nostr-gobject/latency/heartbeat-not-starved",
                    test_heartbeat_not_starved);

    g_test_add_func("/nostr-gobject/latency/heartbeat-detects-blocking",
                    test_heartbeat_detects_blocking);
    g_test_add_func("/nostr-gobject/latency/heartbeat-detects-final-blocking",
                    test_heartbeat_detects_final_blocking);
    g_test_add_func("/nostr-gobject/latency/heartbeat-detects-burst-pressure",
                    test_heartbeat_detects_burst_pressure);
    g_test_add_func("/nostr-gobject/latency/heartbeat-gap-boundary",
                    test_heartbeat_gap_boundary);

    /* Opt-in diagnostic regression, separate from the gating registration and
     * never counted in it. Run by heartbeat-sched-probe.sh. */
    if (env_opt_in("NOSTRC_HEARTBEAT_DIAGNOSTICS"))
        g_test_add_func("/nostr-gobject/latency-diagnostic/sched-env-thread-cpu",
                        test_sched_env_thread_cpu_excludes_sleep);

    return g_test_run();
}
