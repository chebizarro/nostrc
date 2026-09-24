/*
 * nh_porthome_notify.c — unified desktop-notification seam.
 *
 * SPDX-License-Identifier: MIT
 *
 * See nh_porthome_notify.h. Bead: nostrc-h10m.1.
 */

#define _GNU_SOURCE
#include "nh_porthome_notify.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ─────────────── FNV-1a for keying. Cheap, no deps. ─────────────── */
static uint64_t fnv1a(const char *s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    if (!s) return h;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        h ^= *p;
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* ─────────────── throttle ring ─────────────── */
#define NH_NOTIFY_RING_SIZE 64u

typedef struct {
    nh_notify_category cat;
    uint64_t           key_hash;
    int64_t            last_fired;
} nh_notify_slot;

struct nh_porthome_notifier {
    uint32_t             window_secs;
    /* -1 = unset (env/config wins). Otherwise a minute-of-day pair. */
    int                  quiet_start_min;
    int                  quiet_end_min;
    int                  quiet_from_override; /* set_quiet_hours() wins */

    nh_notify_backend_fn backend_fn;
    void                *backend_ud;

    nh_notify_clock_fn   clock_fn;
    void                *clock_ud;

    nh_notify_record_fn  record_fn;
    void                *record_ud;

    nh_notify_slot       ring[NH_NOTIFY_RING_SIZE];
    size_t               ring_next; /* insertion pointer for evictions */
};

/* ─────────────── quiet-hours parsing ─────────────── */
int nh_porthome_notify_parse_hhmm(const char *s,
                                  int *out_start_min,
                                  int *out_end_min) {
    if (!s || !out_start_min || !out_end_min) return -1;
    /* Trim leading whitespace. */
    while (*s && isspace((unsigned char)*s)) ++s;
    /* Empty / explicit "off". */
    if (!*s || !strcmp(s, "off") || !strcmp(s, "OFF")) {
        *out_start_min = -1;
        *out_end_min   = -1;
        return 0;
    }
    int h1, m1, h2, m2;
    /* Accept optional whitespace around the dash. */
    if (sscanf(s, " %d:%d - %d:%d", &h1, &m1, &h2, &m2) != 4)
        return -1;
    if (h1 < 0 || h1 > 23 || m1 < 0 || m1 > 59) return -1;
    if (h2 < 0 || h2 > 23 || m2 < 0 || m2 > 59) return -1;
    *out_start_min = h1 * 60 + m1;
    *out_end_min   = h2 * 60 + m2;
    return 0;
}

/* Load quiet-hours: env first, then $XDG_CONFIG_HOME/nostr-homed/
 * quiet-hours file. Called at each notify() so runtime changes are
 * picked up without restart. When set_quiet_hours() has been called
 * explicitly (override flag set) the env/config are ignored. */
static void resolve_quiet_hours(const nh_porthome_notifier *n,
                                int *start_min,
                                int *end_min) {
    *start_min = -1;
    *end_min   = -1;
    if (n->quiet_from_override) {
        *start_min = n->quiet_start_min;
        *end_min   = n->quiet_end_min;
        return;
    }
    const char *env = getenv("NOSTR_HOMED_PORTHOME_QUIET_HOURS");
    if (env && *env) {
        if (nh_porthome_notify_parse_hhmm(env, start_min, end_min) == 0)
            return;
        /* Malformed env → fall through to config file. */
    }
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char path[512];
    if (xdg && *xdg) {
        snprintf(path, sizeof path, "%s/nostr-homed/quiet-hours", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) return;
        snprintf(path, sizeof path, "%s/.config/nostr-homed/quiet-hours", home);
    }
    FILE *f = fopen(path, "re");
    if (!f) return;
    char line[128];
    if (fgets(line, sizeof line, f)) {
        /* Strip trailing whitespace / newline. */
        size_t L = strlen(line);
        while (L && isspace((unsigned char)line[L - 1])) line[--L] = 0;
        (void)nh_porthome_notify_parse_hhmm(line, start_min, end_min);
    }
    fclose(f);
}

/* Wall-clock (Unix seconds). Test seam overrides. */
static int64_t clock_now(const nh_porthome_notifier *n) {
    if (n->clock_fn) return n->clock_fn(n->clock_ud);
    return (int64_t)time(NULL);
}

/* Is `now` inside the [start, end] window (both in minutes-of-day)?
 * end < start wraps midnight. Both == -1 → no window. */
static bool in_quiet_window(int64_t now, int start_min, int end_min) {
    if (start_min < 0 || end_min < 0) return false;
    if (start_min == end_min) return false; /* zero-length window */
    struct tm tm;
    time_t t = (time_t)now;
    localtime_r(&t, &tm);
    int cur = tm.tm_hour * 60 + tm.tm_min;
    if (start_min <= end_min) return cur >= start_min && cur < end_min;
    /* Wraps midnight: e.g. 22:00-06:00. */
    return cur >= start_min || cur < end_min;
}

/* Throttle: return true iff (cat,key_hash) has fired within
 * window_secs. Updates the slot on delivery. */
static bool should_throttle(nh_porthome_notifier *n,
                            nh_notify_category cat,
                            uint64_t key_hash,
                            int64_t now) {
    if (n->window_secs == 0) return false;
    for (size_t i = 0; i < NH_NOTIFY_RING_SIZE; ++i) {
        nh_notify_slot *s = &n->ring[i];
        if (s->cat == cat && s->key_hash == key_hash) {
            if (s->last_fired > 0 &&
                now - s->last_fired < (int64_t)n->window_secs)
                return true;
            return false;
        }
    }
    return false;
}

static void remember_fired(nh_porthome_notifier *n,
                           nh_notify_category cat,
                           uint64_t key_hash,
                           int64_t now) {
    for (size_t i = 0; i < NH_NOTIFY_RING_SIZE; ++i) {
        nh_notify_slot *s = &n->ring[i];
        if (s->cat == cat && s->key_hash == key_hash) {
            s->last_fired = now;
            return;
        }
    }
    /* Evict the next round-robin slot. */
    nh_notify_slot *s = &n->ring[n->ring_next % NH_NOTIFY_RING_SIZE];
    n->ring_next = (n->ring_next + 1) % NH_NOTIFY_RING_SIZE;
    s->cat        = cat;
    s->key_hash   = key_hash;
    s->last_fired = now;
}

/* ─────────────── default backend: fork+exec notify-send ─────────── */
static void default_backend(void *ud, const char *app, const char *icon,
                            const char *summary, const char *body) {
    (void)ud;
    /* Env kill-switch (matches the historical nh_syncd_reconcile
     * behaviour so packagers/tests can gate the desktop pop). */
    const char *env = getenv("NOSTR_HOMED_SYNCD_NOTIFY");
    if (env && !strcmp(env, "0")) return;

    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        /* Suppress stderr — notify-send's absence is a soft failure. */
        int dn = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (dn >= 0) { (void)dup2(dn, 2); close(dn); }
        char appname_buf[128];
        char icon_buf[128];
        snprintf(appname_buf, sizeof appname_buf,
                 "--app-name=%s", app ? app : "nostr-home");
        snprintf(icon_buf, sizeof icon_buf,
                 "--icon=%s", icon ? icon : "folder-remote");
        execlp("notify-send", "notify-send",
               appname_buf, icon_buf,
               summary ? summary : "portable home",
               body    ? body    : "",
               (char *)NULL);
        _exit(127);
    }
    /* Reap non-blocking. The libnotify daemon usually returns
     * immediately; a slow one lingering as a zombie is cheap. */
    int status = 0;
    (void)waitpid(pid, &status, WNOHANG);
}

/* ─────────────── public API ─────────────── */

nh_porthome_notifier *nh_porthome_notifier_new(void) {
    nh_porthome_notifier *n = calloc(1, sizeof *n);
    if (!n) {
        /* Fatal — the porthome stack treats OOM as unrecoverable. */
        fprintf(stderr, "nh_porthome_notifier_new: OOM\n");
        abort();
    }
    n->window_secs         = 600u;
    n->quiet_start_min     = -1;
    n->quiet_end_min       = -1;
    n->quiet_from_override = 0;
    n->backend_fn          = default_backend;
    n->backend_ud          = NULL;
    n->clock_fn            = NULL;
    n->clock_ud            = NULL;
    n->record_fn           = NULL;
    n->record_ud           = NULL;
    return n;
}

void nh_porthome_notifier_free(nh_porthome_notifier *n) {
    free(n);
}

void nh_porthome_notifier_set_window(nh_porthome_notifier *n,
                                     uint32_t seconds) {
    if (n) n->window_secs = seconds;
}

void nh_porthome_notifier_set_quiet_hours(nh_porthome_notifier *n,
                                          int start_minutes,
                                          int end_minutes) {
    if (!n) return;
    n->quiet_start_min     = start_minutes;
    n->quiet_end_min       = end_minutes;
    n->quiet_from_override = 1;
}

void nh_porthome_notifier_set_backend(nh_porthome_notifier *n,
                                      nh_notify_backend_fn fn,
                                      void *ud) {
    if (!n) return;
    n->backend_fn = fn ? fn : default_backend;
    n->backend_ud = ud;
}

void nh_porthome_notifier_set_clock(nh_porthome_notifier *n,
                                    nh_notify_clock_fn fn,
                                    void *ud) {
    if (!n) return;
    n->clock_fn = fn;
    n->clock_ud = ud;
}

void nh_porthome_notifier_set_record(nh_porthome_notifier *n,
                                     nh_notify_record_fn fn,
                                     void *ud) {
    if (!n) return;
    n->record_fn = fn;
    n->record_ud = ud;
}

const char *nh_porthome_notify_category_slug(nh_notify_category cat) {
    switch (cat) {
        case NH_NOTIFY_CAT_SWEEP:        return "sweep";
        case NH_NOTIFY_CAT_CONFLICT:     return "conflict";
        case NH_NOTIFY_CAT_LIMITED_MODE: return "limited_mode";
        case NH_NOTIFY_CAT_OFFLINE_MISS: return "offline_miss";
        case NH_NOTIFY_CAT_PROVISION:    return "provision";
        default:                         return "other";
    }
}

int nh_porthome_notify(nh_porthome_notifier *n,
                       const char *app,
                       const char *icon,
                       const char *summary,
                       const char *body,
                       nh_notify_category cat,
                       const char *key) {
    if (!n) return -1;
    if (!summary) return -1;
    if (!app)  app  = "nostr-home";
    if (!icon) icon = "folder-remote";

    int64_t  now      = clock_now(n);
    uint64_t key_hash = fnv1a(key ? key : "");

    /* Throttle scope covers ALL suppression reasons — an event
     * suppressed by the throttle still records to the status file. */
    bool throttled = should_throttle(n, cat, key_hash, now);

    int qs = -1, qe = -1;
    resolve_quiet_hours(n, &qs, &qe);
    bool quiet = in_quiet_window(now, qs, qe);

    bool deliver = !throttled && !quiet;
    if (deliver) {
        n->backend_fn(n->backend_ud, app, icon, summary, body);
        remember_fired(n, cat, key_hash, now);
    }

    if (n->record_fn) {
        n->record_fn(n->record_ud, now, cat,
                     key ? key : "",
                     summary,
                     body ? body : "",
                     deliver);
    }
    return deliver ? 1 : 0;
}
