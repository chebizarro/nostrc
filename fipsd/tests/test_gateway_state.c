/* Ported from upstream src/gateway/state.rs tests (nostrc-iq1n9.3): the pool
 * state file is consumed by its load, private, removed by a failed write,
 * refused when invalid, and kept only on tmpfs (injected here). */
#include "test_gateway.h"
#include "gateway/state.h"
#include <errno.h>
#include <sys/stat.h>

static fipsd_gw_pool_state state(void)
{
    fipsd_gw_pool_state s; memset(&s, 0, sizeof s);
    s.version = 1; snprintf(s.pool, sizeof s.pool, "fd01::/120"); s.total = 255; s.from = 10; s.span = 20; s.hold_secs = 120;
    s.held = calloc(2, sizeof *s.held); CHECK(s.held);
    s.held[0][0] = 1; s.held[0][1] = 3; s.held[1][0] = 7; s.held[1][1] = 7; s.held_len = 2;
    return s;
}
/* A state directory inside a fresh temporary directory, not yet created. */
typedef struct dirs { char root[64], dir[128]; } dirs;
static dirs state_dir(void)
{
    dirs d; snprintf(d.root, sizeof d.root, "/tmp/fipsgw-state-XXXXXX");
    CHECK(mkdtemp(d.root));
    snprintf(d.dir, sizeof d.dir, "%s/fips-gateway", d.root);
    return d;
}
static void remove_tree(const dirs *d)
{
    char p[256];
    snprintf(p, sizeof p, "%s/%s", d->dir, FIPSD_GW_STATE_TMP); (void)rmdir(p); (void)unlink(p);
    snprintf(p, sizeof p, "%s/%s", d->dir, FIPSD_GW_STATE_FILE); (void)unlink(p);
    (void)rmdir(d->dir);
    snprintf(p, sizeof p, "%s/absent", d->root); (void)rmdir(p);
    (void)rmdir(d->root);
}
static int always_tmpfs(const char *path, void *user) { (void)path; (void)user; return 1; }
static int never_tmpfs(const char *path, void *user) { (void)path; (void)user; return 0; }
static int parent_only(const char *path, void *user) { return strcmp(path, (const char *)user) != 0; }
static bool exists(const char *dir, const char *name) { char p[256]; snprintf(p, sizeof p, "%s/%s", dir, name); struct stat st; return stat(p, &st) == 0; }
static int save(const dirs *d, const fipsd_gw_pool_state *s, fipsd_gw_tmpfs_fn fn, void *user)
{ char err[256]; return fipsd_gw_state_save(d->dir, s, fn, user, err, sizeof err); }
static fipsd_gw_loaded_kind load(const dirs *d, const char *cidr, uint32_t total, fipsd_gw_loaded *out)
{ fipsd_gw_state_load(d->dir, cidr, total, out); return out->kind; }

static void a_saved_state_round_trips_and_is_consumed_by_the_load(void)
{
    dirs d = state_dir(); fipsd_gw_pool_state s = state(); fipsd_gw_loaded l;
    CHECK(save(&d, &s, always_tmpfs, NULL) == 0);
    CHECK(load(&d, "fd01::/120", 255, &l) == FIPSD_GW_LOADED_RESTORED);
    CHECK(fipsd_gw_pool_state_eq(&l.state, &s));
    CHECKM(!exists(d.dir, FIPSD_GW_STATE_FILE), "the load removed the file");
    fipsd_gw_loaded_clear(&l);
    CHECK(load(&d, "fd01::/120", 255, &l) == FIPSD_GW_LOADED_ABSENT);
    fipsd_gw_pool_state_clear(&s); remove_tree(&d);
}
static void the_encoding_is_serdes(void)
{
    fipsd_gw_pool_state s = state();
    char *json = fipsd_gw_state_encode(&s);
    CHECK(!strcmp(json, "{\"version\":1,\"pool\":\"fd01::/120\",\"total\":255,\"from\":10,\"span\":20,\"held\":[[1,3],[7,7]],\"hold_secs\":120}"));
    fipsd_gw_pool_state back; char reason[256];
    CHECK(fipsd_gw_state_decode(json, strlen(json), &back, reason, sizeof reason) && fipsd_gw_pool_state_eq(&back, &s));
    fipsd_gw_pool_state_clear(&back);
    const char *dup = "{\"version\":1,\"version\":1,\"pool\":\"x\",\"total\":1,\"from\":1,\"span\":0,\"held\":[],\"hold_secs\":0}";
    CHECK(!fipsd_gw_state_decode(dup, strlen(dup), &back, reason, sizeof reason));
    const char *missing = "{\"version\":1,\"pool\":\"x\",\"total\":1,\"from\":1,\"held\":[],\"hold_secs\":0}";
    CHECK(!fipsd_gw_state_decode(missing, strlen(missing), &back, reason, sizeof reason) && strstr(reason, "span"));
    free(json); fipsd_gw_pool_state_clear(&s);
}
static void a_restored_start_that_ends_before_its_first_write_leaves_its_holds_for_the_next(void)
{
    dirs d = state_dir(); fipsd_gw_pool_state s = state();
    CHECK(save(&d, &s, always_tmpfs, NULL) == 0);
    fipsd_gw_instant now = gw_t0();
    fipsd_gw_started st;
    CHECK(fipsd_gw_state_start_pool(d.dir, "fd01::/120", 255, 300, 300, 99, now, always_tmpfs, NULL, &st) == FIPSD_GW_POOL_OK);
    CHECKM(st.loaded.kind == FIPSD_GW_LOADED_RESTORED && fipsd_gw_pool_state_eq(&st.loaded.state, &s), "control");
    bool carried_ok = st.carried && st.carried_rc == 0;
    fipsd_gw_pool_free(st.pool); fipsd_gw_loaded_clear(&st.loaded);
    fipsd_gw_loaded carried;
    CHECKM(load(&d, "fd01::/120", 255, &carried) == FIPSD_GW_LOADED_RESTORED, "a start that failed after loading the state left none for the next");
    CHECKM(carried_ok, "the write back reported its outcome");
    CHECK(carried.state.held_len == 3);
    CHECK(carried.state.held[0][0] == 1 && carried.state.held[0][1] == 3 && carried.state.held[1][0] == 7 && carried.state.held[1][1] == 7);
    CHECK(carried.state.held[2][0] == 10 && carried.state.held[2][1] == 29);
    CHECK(carried.state.from == 30 && carried.state.span == 0);
    CHECKM(carried.state.hold_secs >= 1 && carried.state.hold_secs <= s.hold_secs + 1, "the hold is carried, not extended: %llu", (unsigned long long)carried.state.hold_secs);
    /* A start that keeps failing holds no more each time. */
    CHECK(save(&d, &carried.state, always_tmpfs, NULL) == 0);
    CHECK(fipsd_gw_state_start_pool(d.dir, "fd01::/120", 255, 300, 300, 99, now + SECS(100), always_tmpfs, NULL, &st) == FIPSD_GW_POOL_OK);
    fipsd_gw_pool_free(st.pool); fipsd_gw_loaded_clear(&st.loaded);
    fipsd_gw_loaded again;
    CHECKM(load(&d, "fd01::/120", 255, &again) == FIPSD_GW_LOADED_RESTORED, "the second failed start left no state");
    CHECKM(again.state.hold_secs <= carried.state.hold_secs + 1, "a second failed start re-armed the hold to %llu s from %llu s",
           (unsigned long long)again.state.hold_secs, (unsigned long long)carried.state.hold_secs);
    CHECK(again.state.held_len == carried.state.held_len && again.state.from == 30 && again.state.span == 0);
    fipsd_gw_loaded_clear(&carried); fipsd_gw_loaded_clear(&again); fipsd_gw_pool_state_clear(&s); remove_tree(&d);
}
static void a_fresh_start_writes_no_state_before_its_first_write(void)
{
    dirs d = state_dir();
    fipsd_gw_started st;
    CHECK(fipsd_gw_state_start_pool(d.dir, "fd01::/120", 255, 60, 60, 99, gw_t0(), always_tmpfs, NULL, &st) == FIPSD_GW_POOL_OK);
    CHECK(st.loaded.kind == FIPSD_GW_LOADED_ABSENT && !st.carried && !exists(d.dir, FIPSD_GW_STATE_FILE));
    CHECKM(fipsd_gw_pool_test_start(st.pool) == 99, "fresh at the random offset");
    fipsd_gw_pool_free(st.pool); remove_tree(&d);
}
static void the_state_file_and_directory_are_private(void)
{
    dirs d = state_dir(); fipsd_gw_pool_state s = state();
    CHECK(save(&d, &s, always_tmpfs, NULL) == 0);
    struct stat st; char p[256];
    CHECK(stat(d.dir, &st) == 0 && (st.st_mode & 0777) == 0700);
    snprintf(p, sizeof p, "%s/%s", d.dir, FIPSD_GW_STATE_FILE);
    CHECK(stat(p, &st) == 0 && (st.st_mode & 0777) == 0600);
    fipsd_gw_pool_state_clear(&s); remove_tree(&d);
}
static void a_failed_save_after_a_successful_one_leaves_no_state(void)
{
    dirs d = state_dir(); fipsd_gw_pool_state s = state(); fipsd_gw_loaded l;
    CHECK(save(&d, &s, always_tmpfs, NULL) == 0);
    char p[256]; snprintf(p, sizeof p, "%s/%s", d.dir, FIPSD_GW_STATE_TMP);
    CHECK(mkdir(p, 0700) == 0); /* the next write cannot create its temporary file */
    CHECK(save(&d, &s, always_tmpfs, NULL) != 0);
    CHECKM(load(&d, "fd01::/120", 255, &l) == FIPSD_GW_LOADED_ABSENT, "an older state survived a failed write");
    fipsd_gw_pool_state_clear(&s); remove_tree(&d);
}
static void a_save_whose_directory_is_not_tmpfs_on_the_second_check_leaves_no_state(void)
{
    dirs d = state_dir(); fipsd_gw_pool_state s = state(); fipsd_gw_loaded l;
    CHECK(save(&d, &s, always_tmpfs, NULL) == 0);
    CHECK(save(&d, &s, parent_only, d.dir) != 0);
    CHECK(load(&d, "fd01::/120", 255, &l) == FIPSD_GW_LOADED_ABSENT);
    fipsd_gw_pool_state_clear(&s); remove_tree(&d);
}
static void a_load_followed_by_a_failed_save_leaves_no_file(void)
{
    dirs d = state_dir(); fipsd_gw_pool_state s = state(); fipsd_gw_loaded l;
    CHECK(save(&d, &s, always_tmpfs, NULL) == 0);
    CHECK(load(&d, "fd01::/120", 255, &l) == FIPSD_GW_LOADED_RESTORED); fipsd_gw_loaded_clear(&l);
    char p[256]; snprintf(p, sizeof p, "%s/%s", d.dir, FIPSD_GW_STATE_TMP);
    CHECK(mkdir(p, 0700) == 0);
    CHECK(save(&d, &s, always_tmpfs, NULL) != 0);
    CHECK(!exists(d.dir, FIPSD_GW_STATE_FILE));
    CHECK(load(&d, "fd01::/120", 255, &l) == FIPSD_GW_LOADED_ABSENT);
    fipsd_gw_pool_state_clear(&s); remove_tree(&d);
}
static void a_truncated_file_is_invalid_and_starts_fresh_at_the_random_offset(void)
{
    dirs d = state_dir(); fipsd_gw_pool_state s = state(); fipsd_gw_loaded l;
    CHECK(save(&d, &s, always_tmpfs, NULL) == 0);
    char p[256]; snprintf(p, sizeof p, "%s/%s", d.dir, FIPSD_GW_STATE_FILE);
    char *json = fipsd_gw_state_encode(&s);
    FILE *f = fopen(p, "w"); CHECK(f); fwrite(json, 1, strlen(json) / 2, f); fclose(f);
    CHECK(load(&d, "fd01::/120", 255, &l) == FIPSD_GW_LOADED_INVALID);
    fipsd_gw_pool_begin b; fipsd_gw_state_start_from(&l, 77, &b);
    CHECK(!b.restored && b.offset == 77);
    CHECKM(!exists(d.dir, FIPSD_GW_STATE_FILE), "an invalid file is consumed too");
    free(json); fipsd_gw_pool_state_clear(&s); remove_tree(&d);
}
static void expect_invalid(fipsd_gw_pool_state *s, const char *cidr, uint32_t total)
{
    dirs d = state_dir(); fipsd_gw_loaded l;
    CHECK(save(&d, s, always_tmpfs, NULL) == 0);
    CHECKM(load(&d, cidr, total, &l) == FIPSD_GW_LOADED_INVALID, "accepted: %s", l.reason);
    fipsd_gw_loaded_clear(&l); remove_tree(&d);
}
static void a_state_for_another_pool_is_invalid(void) { fipsd_gw_pool_state s = state(); expect_invalid(&s, "fd01::/112", 65535); fipsd_gw_pool_state_clear(&s); }
static void a_stretch_longer_than_the_pool_is_invalid(void) { fipsd_gw_pool_state s = state(); s.span = 256; expect_invalid(&s, "fd01::/120", 255); fipsd_gw_pool_state_clear(&s); }
static void overlapping_held_ranges_are_invalid(void)
{
    fipsd_gw_pool_state s = state();
    s.held[0][0] = 1; s.held[0][1] = 5; s.held[1][0] = 5; s.held[1][1] = 9;
    expect_invalid(&s, "fd01::/120", 255); fipsd_gw_pool_state_clear(&s);
}
static void a_held_range_ending_past_the_pool_is_invalid(void)
{
    fipsd_gw_pool_state s = state();
    s.held[1][0] = 250; s.held[1][1] = 256;
    expect_invalid(&s, "fd01::/120", 255);
    s.held[1][1] = 255;
    dirs d = state_dir(); fipsd_gw_loaded l;
    CHECK(save(&d, &s, always_tmpfs, NULL) == 0);
    CHECKM(load(&d, "fd01::/120", 255, &l) == FIPSD_GW_LOADED_RESTORED, "control: the same range ending at the last offset is accepted");
    fipsd_gw_loaded_clear(&l); remove_tree(&d); fipsd_gw_pool_state_clear(&s);
}
static void an_unbounded_hold_is_invalid_and_starts_fresh_without_panicking(void)
{
    fipsd_gw_pool_state s = state(); s.hold_secs = UINT64_MAX;
    dirs d = state_dir(); fipsd_gw_loaded l;
    CHECK(save(&d, &s, always_tmpfs, NULL) == 0);
    CHECK(load(&d, "fd01::/120", 255, &l) == FIPSD_GW_LOADED_INVALID);
    fipsd_gw_pool_begin b; fipsd_gw_state_start_from(&l, 5, &b);
    CHECK(!b.restored && b.offset == 5);
    /* Just inside the bound, the state is valid. */
    s.hold_secs = FIPSD_GW_RESTART_HOLD_MAX_SECS;
    char reason[256]; CHECK(fipsd_gw_state_validate(&s, "fd01::/120", 255, reason, sizeof reason));
    s.hold_secs = FIPSD_GW_RESTART_HOLD_MAX_SECS + 1;
    CHECK(!fipsd_gw_state_validate(&s, "fd01::/120", 255, reason, sizeof reason) && strstr(reason, "exceeds"));
    remove_tree(&d); fipsd_gw_pool_state_clear(&s);
}
static void a_missing_parent_fails_the_save_and_creates_nothing(void)
{
    dirs d = state_dir(); fipsd_gw_pool_state s = state();
    snprintf(d.dir, sizeof d.dir, "%s/absent/fips-gateway", d.root);
    CHECK(save(&d, &s, always_tmpfs, NULL) == ENOENT);
    char p[256]; snprintf(p, sizeof p, "%s/absent", d.root);
    struct stat st; CHECK(stat(p, &st) != 0);
    fipsd_gw_pool_state_clear(&s); remove_tree(&d);
}
static void a_parent_not_on_tmpfs_fails_the_save_and_creates_nothing(void)
{
    dirs d = state_dir(); fipsd_gw_pool_state s = state();
    char err[256];
    CHECK(fipsd_gw_state_save(d.dir, &s, never_tmpfs, NULL, err, sizeof err) != 0);
    CHECKM(strstr(err, "not on tmpfs"), "%s", err);
    struct stat st; CHECKM(stat(d.dir, &st) != 0, "no directory was created on other storage");
    fipsd_gw_pool_state_clear(&s); remove_tree(&d);
}
static void the_save_log_warns_once_per_change_of_outcome(void)
{
    fipsd_gw_save_log log = {0};
    CHECK(fipsd_gw_save_log_report(&log, 0, NULL) == FIPSD_GW_SAVE_OK);
    CHECK(fipsd_gw_save_log_report(&log, EXDEV, "x is not on tmpfs") == FIPSD_GW_SAVE_FAILED);
    CHECK(fipsd_gw_save_log_report(&log, EXDEV, "x is not on tmpfs") == FIPSD_GW_SAVE_REPEATED);
    CHECK(fipsd_gw_save_log_report(&log, ENOSPC, "no space") == FIPSD_GW_SAVE_FAILED);
    CHECK(fipsd_gw_save_log_report(&log, 0, NULL) == FIPSD_GW_SAVE_RECOVERED);
    CHECK(fipsd_gw_save_log_report(&log, 0, NULL) == FIPSD_GW_SAVE_OK);
}
#ifdef __linux__
static void proc_is_not_tmpfs(void) { CHECK(fipsd_gw_is_tmpfs("/proc", NULL) == 0); }
static void is_tmpfs_is_true_for_a_mounted_tmpfs(void)
{
    FILE *f = fopen("/proc/self/mounts", "r"); CHECK(f);
    char line[1024], found[512] = "";
    while (fgets(line, sizeof line, f)) {
        char dev[256], dir[512], type[64];
        if (sscanf(line, "%255s %511s %63s", dev, dir, type) != 3 || strcmp(type, "tmpfs")) continue;
        if (fipsd_gw_is_tmpfs(dir, NULL) < 0) continue;
        if (!*found || !strcmp(dir, "/dev/shm") || !strcmp(dir, "/run")) snprintf(found, sizeof found, "%s", dir);
    }
    fclose(f);
    CHECKM(*found, "no accessible tmpfs mount is listed in /proc/self/mounts to check against");
    CHECKM(fipsd_gw_is_tmpfs(found, NULL) == 1, "%s is listed as tmpfs", found);
}
#endif

int gw_suite_state(void)
{
    RUN(a_saved_state_round_trips_and_is_consumed_by_the_load);
    RUN(the_encoding_is_serdes);
    RUN(a_restored_start_that_ends_before_its_first_write_leaves_its_holds_for_the_next);
    RUN(a_fresh_start_writes_no_state_before_its_first_write);
    RUN(the_state_file_and_directory_are_private);
    RUN(a_failed_save_after_a_successful_one_leaves_no_state);
    RUN(a_save_whose_directory_is_not_tmpfs_on_the_second_check_leaves_no_state);
    RUN(a_load_followed_by_a_failed_save_leaves_no_file);
    RUN(a_truncated_file_is_invalid_and_starts_fresh_at_the_random_offset);
    RUN(a_state_for_another_pool_is_invalid);
    RUN(a_stretch_longer_than_the_pool_is_invalid);
    RUN(overlapping_held_ranges_are_invalid);
    RUN(a_held_range_ending_past_the_pool_is_invalid);
    RUN(an_unbounded_hold_is_invalid_and_starts_fresh_without_panicking);
    RUN(a_missing_parent_fails_the_save_and_creates_nothing);
    RUN(a_parent_not_on_tmpfs_fails_the_save_and_creates_nothing);
    RUN(the_save_log_warns_once_per_change_of_outcome);
#ifdef __linux__
    RUN(proc_is_not_tmpfs);
    RUN(is_tmpfs_is_true_for_a_mounted_tmpfs);
#endif
    return 0;
}
