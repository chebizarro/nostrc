#include "instr/recorder.h"
#include "instr/writer.h"
#include "node/perf_profile.h"
#include "control/protocol.h"
#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#ifndef FIPSD_PROFILE_LIB_EMITS_DEBUG_ROW
#error "FIPSD_PROFILE_LIB_EMITS_DEBUG_ROW is defined by fipsd/CMakeLists.txt"
#endif

/* Test-local syscall seam, matching the existing logger fault-object pattern.
 * These hooks are never part of the daemon/library. */
static int create_fault, captured_fd;
static pthread_mutex_t fault_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fault_done = PTHREAD_COND_INITIALIZER;
static bool fault_finished;
static void *(*fault_run)(void *);
static void *fault_arg;
int fipsd_profile_test_condwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *until);
int fipsd_profile_test_pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
int fipsd_profile_test_condwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *until)
{
    if (create_fault == 2) return ETIMEDOUT;
    return pthread_cond_timedwait(c, m, until);
}
static void *fail_writer(void *unused)
{
    (void)unused;
    assert(freopen("/dev/null", "r", fault_arg)); /* real EBADF on output */
    void *out = fault_run(fault_arg);
    pthread_mutex_lock(&fault_mu); fault_finished = true;
    pthread_cond_signal(&fault_done); pthread_mutex_unlock(&fault_mu);
    return out;
}
int fipsd_profile_test_pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
    if (create_fault == 1) { captured_fd = fileno(arg); return EAGAIN; }
    if (create_fault != 2) return pthread_create(t, attr, fn, arg);
    pthread_mutex_lock(&fault_mu);
    fault_finished = false; fault_run = fn; fault_arg = arg;
    int rc = pthread_create(t, attr, fail_writer, NULL);
    if (!rc) while (!fault_finished) pthread_cond_wait(&fault_done, &fault_mu);
    pthread_mutex_unlock(&fault_mu);
    return rc; /* writer has failed before start() regains control */
}

static char *contents(FILE *f)
{
    assert(fflush(f) == 0); assert(fseek(f, 0, SEEK_END) == 0);
    long n = ftell(f); assert(n >= 0); rewind(f);
    char *text = calloc((size_t)n + 1, 1); assert(text);
    assert(fread(text, 1, (size_t)n, f) == (size_t)n); return text;
}
static void recorder(void)
{
    fipsd_instr_reset(); fipsd_instr_arm(true);
    FIPSD_INSTR_GATE(on);
    int calls = 0;
    FIPSD_INSTR_STEP(on, WHOLE_TICK,
        FIPSD_INSTR_STEP(on, CHECK_TIMEOUTS, ++calls));
    assert(calls == 1);
    fipsd_instr_sample child = fipsd_instr_take_step(FIPSD_INSTR_CHECK_TIMEOUTS);
    fipsd_instr_sample parent = fipsd_instr_take_step(FIPSD_INSTR_WHOLE_TICK);
    assert(child.count == 1 && parent.count == 1 && parent.total >= child.total);
    assert(fipsd_instr_take_step(FIPSD_INSTR_WHOLE_TICK).count == 0);
    /* No finite span stack/buffer to overflow. Counters use upstream's u64
     * wrapping arithmetic even at their limit; maximum remains exact. */
    fipsd_instr_record(FIPSD_INSTR_POLL_PROBES, UINT64_MAX);
    fipsd_instr_record(FIPSD_INSTR_POLL_PROBES, 2);
    fipsd_instr_sample s = fipsd_instr_take_step(FIPSD_INSTR_POLL_PROBES);
    assert(s.count == 2 && s.max == UINT64_MAX && s.total == 1);
    for (int i = 0; i < 100000; ++i) fipsd_instr_record(FIPSD_INSTR_POLL_PROBES, 7);
    s = fipsd_instr_take_step(FIPSD_INSTR_POLL_PROBES);
    assert(s.count == 100000 && s.total == 700000 && s.max == 7);
    fipsd_instr_tick_entry(1000, 50);
    assert(fipsd_instr_take_gauge(FIPSD_INSTR_TICK_GAP).count == 0);
    fipsd_instr_tick_entry(2000, 50); fipsd_instr_tick_entry(3000, 50);
    s = fipsd_instr_take_gauge(FIPSD_INSTR_ARM_STARVATION);
    assert(s.count == 3 && s.max == 50 && s.total == 150);
    s = fipsd_instr_take_gauge(FIPSD_INSTR_TICK_GAP);
    assert(s.count == 2 && s.max == 1000 && s.total == 2000);
    fipsd_instr_reset();
    for (int i = 0; i < FIPSD_INSTR_STEPS; ++i) assert(fipsd_instr_take_step(i).count == 0);
    for (int i = 0; i < FIPSD_INSTR_GAUGES; ++i) assert(fipsd_instr_take_gauge(i).count == 0);
    fipsd_instr_tick_entry(4000, 0);
    assert(fipsd_instr_take_gauge(FIPSD_INSTR_TICK_GAP).count == 0);
    fipsd_instr_arm(false);
    FIPSD_INSTR_GATE(off);
    FIPSD_INSTR_STEP(off, WHOLE_TICK, ++calls);
    assert(calls == 2 && fipsd_instr_take_step(FIPSD_INSTR_WHOLE_TICK).count == 0);
}
/* Normalize values, not schema. The reference is an actual upstream release
 * capture; debug's extra invariant row is checked separately below. */
static char *shape(char *text)
{
    char *out = calloc(strlen(text) + 1, 1); assert(out);
    char *line_save = NULL;
    for (char *line = strtok_r(text, "\n", &line_save); line; line = strtok_r(NULL, "\n", &line_save)) {
        if (strstr(line, "\tdebug_assert_peer_maps_coherent\t")) continue;
        if (!strncmp(line, "# NOTE", 6) || !strncmp(line, "ts_unix", 7) || !strchr(line, '\t')) {
            strcat(out, line); strcat(out, "\n"); continue;
        }
        if (line[0] == '#') { *strchr(line, '\t') = 0; strcat(out, line); strcat(out, "\n"); continue; }
        char *save = NULL, *fields[8]; size_t n = 0;
        for (char *f = strtok_r(line, "\t", &save); f; f = strtok_r(NULL, "\t", &save)) {
            assert(n < 8); fields[n++] = f;
        }
        assert(n == 8);
        for (size_t i = 0; i < 8; ++i) {
            if (i == 0 || (i >= 4 && i <= 6)) {
                char *end; (void)strtoull(fields[i], &end, 10); assert(*end == 0);
                strcat(out, "N");
            } else strcat(out, fields[i]);
            strcat(out, i == 7 ? "\n" : "\t");
        }
    }
    return out;
}
static void generations(void)
{
    fipsd_instr_reset(); fipsd_instr_arm(true);
    uint64_t old = fipsd_instr_gate(); assert(old);
    fipsd_instr_arm(false); fipsd_instr_reset(); fipsd_instr_arm(true);
    uint64_t current = fipsd_instr_gate(); assert(current && current != old);
    /* A paused old span and its gauges cannot enter the next capture. */
    fipsd_instr_record_at(old, FIPSD_INSTR_WHOLE_TICK, 99);
    fipsd_instr_entry_at(old, 1234, 88);
    fipsd_instr_gauge_at(old, FIPSD_INSTR_PEERS, 42);
    assert(fipsd_instr_take_step(FIPSD_INSTR_WHOLE_TICK).count == 0);
    for (int i = 0; i < FIPSD_INSTR_GAUGES; ++i) assert(fipsd_instr_take_gauge(i).count == 0);
    fipsd_instr_record_at(current, FIPSD_INSTR_WHOLE_TICK, 7);
    assert(fipsd_instr_take_step(FIPSD_INSTR_WHOLE_TICK).total == 7);
    fipsd_instr_reset();
}
static void golden(void)
{
    fipsd_instr_reset();
    FILE *f = tmpfile(); assert(f);
    assert(fipsd_instr_write_header(f, "test", 1, 1700000000) > 0);
    fipsd_instr_record(FIPSD_INSTR_CHECK_TIMEOUTS, 2999);
    fipsd_instr_gauge_record(FIPSD_INSTR_PEERS, 42);
    assert(fipsd_instr_flush(f, 1700000001) == 0);
    char *text = contents(f); fclose(f);
    assert(strstr(text, "\tcheck_timeouts\t1\t2\t2\tus\n"));
    assert(strstr(text, "\tpeers\t1\t42\t42\tpeers\n"));
    /* Debug-only row, keyed on the library's NDEBUG (this TU is -UNDEBUG). */
    assert(fipsd_instr_step_emitted(FIPSD_INSTR_DEBUG_ASSERT_PEER_MAPS_COHERENT)
           == (FIPSD_PROFILE_LIB_EMITS_DEBUG_ROW != 0));
#if FIPSD_PROFILE_LIB_EMITS_DEBUG_ROW
    assert(strstr(text, "\tdebug_assert_peer_maps_coherent\t"));
#else
    assert(!strstr(text, "\tdebug_assert_peer_maps_coherent\t"));
#endif
    FILE *ref = fopen(FIPSD_PROFILE_FIXTURES "/rust-tick-release.tsv", "r"); assert(ref);
    char *rust = contents(ref); fclose(ref);
    char *a = shape(text), *b = shape(rust);
    if (strcmp(a, b)) fprintf(stderr, "C shape:\n%sRust shape:\n%s", a, b);
    assert(!strcmp(a, b)); free(a); free(b); free(text); free(rust);
}
static void pipeline(void)
{
    assert(fipsd_perf_enabled());
    assert(fipsd_perf_bucket(0) == 0 && fipsd_perf_bucket(1) == 0);
    assert(fipsd_perf_bucket(2) == 1 && fipsd_perf_bucket(3) == 2);
    assert(fipsd_perf_bucket(UINT64_MAX) == 47);
    uint64_t hist[48] = {0}; hist[0] = 1; hist[2] = 18; hist[10] = 1;
    assert(fipsd_perf_percentile(hist, 20, 50) == 4);
    assert(fipsd_perf_percentile(hist, 20, 95) == 4);
    assert(fipsd_perf_percentile(hist, 20, 99) == 1024);
    assert(fipsd_perf_percentile(hist, 0, 99) == 0);
    fipsd_perf_record(FIPSD_PERF_FMP_ENCRYPT, 0);
    fipsd_perf_record(FIPSD_PERF_FMP_ENCRYPT, 3000);
    fipsd_perf_event_count(FIPSD_PERF_UDP_SEND_WILDCARD, 4);
    fipsd_perf_snapshot prev = {0}; FILE *f = tmpfile(); assert(f);
    assert(fipsd_perf_report(f, 2, &prev) == 0);
    char *text = contents(f); fclose(f);
    assert(!strcmp(text, "[pipe 2s] fmp_encrypt=1/s avg=1.5us p50<=1ns p95<=4.1us p99<=4.1us max<=4.1us allmax=3.0us udp_send_wildcard=2/s\n"));
    free(text); f = tmpfile(); assert(f);
    assert(fipsd_perf_report(f, 2, &prev) == 0); text = contents(f); fclose(f);
    assert(!strcmp(text, "[pipe 2s]\n")); free(text);
}
static void *producer(void *arg)
{
    (void)arg;
    for (int i = 0; i < 100000; ++i) {
        fipsd_instr_record(FIPSD_INSTR_WHOLE_TICK, 1234);
        fipsd_perf_record(FIPSD_PERF_FMP_ENCRYPT, 33);
    }
    return NULL;
}
static void concurrent(void)
{
    pthread_t t; assert(pthread_create(&t, NULL, producer, NULL) == 0);
    uint64_t count = 0, total = 0;
    for (int i = 0; i < 10000; ++i) {
        fipsd_instr_sample s = fipsd_instr_take_step(FIPSD_INSTR_WHOLE_TICK);
        count += s.count; total += s.total;
    }
    pthread_join(t, NULL);
    fipsd_instr_sample s = fipsd_instr_take_step(FIPSD_INSTR_WHOLE_TICK);
    assert(count + s.count == 100000 && total + s.total == 123400000);
}
static void capture(void)
{
    /* Root confinement is exercised by the real control test; never write to
     * the developer's /var/log. These tests normally run unprivileged. */
    if (geteuid() == 0) return;
    char dir[] = "profile-test-XXXXXX"; assert(mkdtemp(dir));
    json_t *params = json_pack("{s:s}", "dir", dir);
    create_fault = 1;
    json_t *failed = fipsd_instr_command("profile_tick_on", params, "test", 1);
    assert(!strcmp(json_string_value(json_object_get(failed, "status")), "error"));
    assert(!fipsd_instr_gate());
    errno = 0; assert(fcntl(captured_fd, F_GETFD) == -1 && errno == EBADF);
    json_decref(failed);
    create_fault = 2;
    failed = fipsd_instr_command("profile_tick_on", params, "test", 1);
    assert(!fipsd_instr_gate()); /* terminal writer cannot be re-armed by startup */
    const char *failed_path = json_string_value(json_object_get(json_object_get(failed, "data"), "path"));
    assert(failed_path); char *failed_copy = strdup(failed_path); assert(failed_copy);
    json_t *failure_status = fipsd_instr_command("profile_tick_status", NULL, NULL, 1);
    assert(!strcmp(json_string_value(json_object_get(json_object_get(failure_status, "data"), "state")), "stopped_by_error"));
    json_t *failure_stop = fipsd_instr_command("profile_tick_off", NULL, NULL, 1);
    assert(json_is_true(json_object_get(json_object_get(failure_stop, "data"), "stopped_by_error")));
    create_fault = 0; assert(unlink(failed_copy) == 0); free(failed_copy);
    json_decref(failed); json_decref(failure_status); json_decref(failure_stop);
    json_t *on = fipsd_instr_command("profile_tick_on", params, "test", 1); assert(on);
    json_t *data = json_object_get(on, "data"); assert(data);
    char *path = strdup(json_string_value(json_object_get(data, "path"))); assert(path);
    struct stat st; assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    json_t *twice = fipsd_instr_command("profile_tick_on", params, "test", 1);
    assert(!strcmp(json_string_value(json_object_get(twice, "status")), "error")); json_decref(twice);
    fipsd_instr_record(FIPSD_INSTR_WHOLE_TICK, 123000);
    json_t *off = fipsd_instr_command("profile_tick_off", NULL, "test", 1);
    assert(json_is_true(json_object_get(json_object_get(off, "data"), "stopped")));
    FILE *f = fopen(path, "r"); assert(f); char *text = contents(f); fclose(f);
    assert(strstr(text, "\twhole_tick\t1\t123\t123\tus\n")); free(text);
    json_t *again = fipsd_instr_command("profile_tick_on", params, "test", 1);
    const char *next = json_string_value(json_object_get(json_object_get(again, "data"), "path"));
    assert(next && strcmp(next, path)); char *path2 = strdup(next); assert(path2);
    fipsd_instr_shutdown(); fipsd_instr_shutdown();
    json_t *status = fipsd_instr_command("profile_tick_status", NULL, NULL, 1);
    data = json_object_get(status, "data");
    assert(!strcmp(json_string_value(json_object_get(data, "state")), "idle"));
    assert(json_is_null(json_object_get(data, "path")) && json_integer_value(json_object_get(data, "bytes")) == 0);
    json_decref(status); json_decref(on); json_decref(off); json_decref(again); json_decref(params);
    assert(unlink(path) == 0 && unlink(path2) == 0 && rmdir(dir) == 0); free(path); free(path2);
}
int main(void)
{
    recorder(); generations(); golden(); pipeline(); concurrent(); capture();
    puts("profiling: nesting overflow reset gauges golden histogram concurrent capture PASS");
    return 0;
}
