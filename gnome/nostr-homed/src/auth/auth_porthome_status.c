/*
 * auth_porthome_status.c — see auth_porthome_status.h.
 *
 * SPDX-License-Identifier: MIT
 *
 * Compiled only when NH_AUTH_BROKER_ENABLE_PORTHOME is defined; the
 * #else stubs below keep the base link closure clean when the operator
 * has not enabled the experimental portable-home broker glue.
 *
 * Bead: nostrc-3o91 (Phase 5 h10m follow-up).
 */
#define _GNU_SOURCE
#include "auth_porthome_status.h"

#ifdef NH_AUTH_BROKER_ENABLE_PORTHOME

#include "nh_porthome_notify.h"
#include "nh_porthome_status.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

/* ─────────────── writer ─────────────── */

struct nh_provisioner_status_writer {
    pthread_mutex_t         mu;
    nh_provisioner_state    state;
    nh_provisioner_state    last_state;
    int64_t                 last_provisioned_ts;
    char                    last_error_class[32];
    uint32_t                chunks_done;
    uint32_t                chunks_pending;
};

nh_provisioner_status_writer *nh_provisioner_status_writer_new(void) {
    nh_provisioner_status_writer *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    pthread_mutex_init(&w->mu, NULL);
    w->state       = NH_PROV_ST_IDLE;
    w->last_state  = NH_PROV_ST_IDLE;
    return w;
}

void nh_provisioner_status_writer_free(nh_provisioner_status_writer *w) {
    if (!w) return;
    pthread_mutex_destroy(&w->mu);
    free(w);
}

const char *nh_provisioner_state_slug(nh_provisioner_state s) {
    switch (s) {
        case NH_PROV_ST_IDLE:       return "idle";
        case NH_PROV_ST_PREPARING:  return "preparing";
        case NH_PROV_ST_FETCHING:   return "fetching";
        case NH_PROV_ST_VERIFYING:  return "verifying";
        case NH_PROV_ST_PUBLISHING: return "publishing";
        case NH_PROV_ST_DONE:       return "done";
        case NH_PROV_ST_ERROR:      return "error";
    }
    return "idle";
}

#define WITH_LOCK(w, body) do { \
    if (!(w)) return; \
    pthread_mutex_lock(&(w)->mu); \
    body; \
    pthread_mutex_unlock(&(w)->mu); \
} while (0)

void nh_provisioner_status_set_state(nh_provisioner_status_writer *w,
                                     nh_provisioner_state s) {
    WITH_LOCK(w, {
        /* Track the previous state as `last_state` so the UI can
         * distinguish "just finished at ts=X, currently idle" from
         * "never provisioned". Only meaningful across a terminal
         * transition; we always record it so consumers can read it. */
        w->last_state = w->state;
        w->state = s;
    });
}

void nh_provisioner_status_set_last_provisioned_ts(
    nh_provisioner_status_writer *w, int64_t epoch_secs) {
    WITH_LOCK(w, { w->last_provisioned_ts = epoch_secs; });
}

void nh_provisioner_status_set_last_error_class(
    nh_provisioner_status_writer *w, const char *class_slug) {
    WITH_LOCK(w, {
        snprintf(w->last_error_class, sizeof w->last_error_class, "%s",
                 class_slug ? class_slug : "");
    });
}

void nh_provisioner_status_set_chunks(nh_provisioner_status_writer *w,
                                      uint32_t done, uint32_t pending) {
    WITH_LOCK(w, {
        w->chunks_done    = done;
        w->chunks_pending = pending;
    });
}

/* Best-effort chown; on failure log at INFO and continue (the file is
 * still valid for the broker itself, and the user session will fall
 * back to the syncd's own writer for its half of the document). */
static void best_effort_chown(const char *path, uid_t uid, gid_t gid) {
    if (!path || !*path) return;
    if (chown(path, uid, gid) != 0)
        syslog(LOG_INFO,
               "porthome/prov-status: chown(%s,%u,%u) failed errno=%d",
               path, (unsigned)uid, (unsigned)gid, errno);
}

int nh_provisioner_status_emit(nh_provisioner_status_writer *w,
                               const char *homedir,
                               uid_t uid, gid_t gid,
                               const char *homedir_override) {
    if (!w) return -EINVAL;

    /* Snapshot under the lock so the render is consistent. */
    pthread_mutex_lock(&w->mu);
    nh_provisioner_state st  = w->state;
    nh_provisioner_state ls  = w->last_state;
    int64_t              lts = w->last_provisioned_ts;
    char err[32]; memcpy(err, w->last_error_class, sizeof err);
    uint32_t cd = w->chunks_done;
    uint32_t cp = w->chunks_pending;
    pthread_mutex_unlock(&w->mu);

    char err_esc[80] = {0};
    (void)nh_porthome_json_escape(err, err_esc, sizeof err_esc);

    char body[512];
    int n = snprintf(body, sizeof body,
        "{\"state\":\"%s\","
         "\"last_state\":\"%s\","
         "\"last_provisioned_ts\":%" PRId64 ","
         "\"last_error_class\":\"%s\","
         "\"chunks_pending\":%u,"
         "\"chunks_done\":%u}",
        nh_provisioner_state_slug(st),
        nh_provisioner_state_slug(ls),
        lts, err_esc, cp, cd);
    if (n < 0 || (size_t)n >= sizeof body) return -ENAMETOOLONG;

    const char *hd = (homedir_override && *homedir_override) ? homedir_override
                                                             : homedir;
    if (!hd || !*hd) return -EINVAL;

    /* Compose the same layout the porthome-common default resolver
     * would produce for that user: <hd>/.local/state/nostr-homed/
     * porthome-status.json. */
    char path[1024];
    int pn = snprintf(path, sizeof path,
        "%s/.local/state/nostr-homed/porthome-status.json", hd);
    if (pn < 0 || (size_t)pn >= sizeof path) return -ENAMETOOLONG;

    /* Ensure the .../nostr-homed dir exists with a permissive mode we
     * can chown from — write_key does mkdir_p at 0700 but the created
     * dirs will land owned by root; chown them to the account so the
     * user's syncd + CLI can traverse. */
    int rc = nh_porthome_status_write_key(path, "provisioner", body);
    if (rc != 0) return rc;

    /* Best-effort chowns: file itself, its lock companion, and the
     * parent nostr-homed dir. When the test path (homedir_override)
     * points at a tmpdir we skip these if uid==0. */
    if (uid != 0) {
        best_effort_chown(path, uid, gid);
        char lockp[1100];
        if (snprintf(lockp, sizeof lockp, "%s.lock", path) < (int)sizeof lockp)
            best_effort_chown(lockp, uid, gid);
        char dir[1024];
        snprintf(dir, sizeof dir, "%s/.local/state/nostr-homed", hd);
        best_effort_chown(dir, uid, gid);
    }
    return 0;
}

/* ─────────────── notifier plumbing ─────────────── */

/* Lazily-initialised broker-scoped notifier. One instance is enough —
 * the throttle window is per-(cat,key), so multiple concurrent jobs
 * still get sane suppression. Never freed (broker lifetime). */
static pthread_once_t   g_notifier_once = PTHREAD_ONCE_INIT;
static nh_porthome_notifier *g_notifier;

static void notifier_init(void) {
    g_notifier = nh_porthome_notifier_new();
}

void nh_provisioner_notify(nh_provisioner_status_writer *w,
                           const char *event_slug,
                           const char *summary,
                           const char *body) {
    (void)w;  /* Reserved for future per-job notifier scoping. */
    if (!event_slug || !*event_slug) return;
    pthread_once(&g_notifier_once, notifier_init);
    if (!g_notifier) return;
    (void)nh_porthome_notify(g_notifier,
                             /* app  */ "nostr-home",
                             /* icon */ "user-home",
                             summary ? summary : event_slug,
                             body    ? body    : "",
                             NH_NOTIFY_CAT_PROVISION,
                             event_slug);
}

#else  /* !NH_AUTH_BROKER_ENABLE_PORTHOME */

nh_provisioner_status_writer *nh_provisioner_status_writer_new(void) {
    return NULL;
}
void nh_provisioner_status_writer_free(nh_provisioner_status_writer *w) {
    (void)w;
}
const char *nh_provisioner_state_slug(nh_provisioner_state s) {
    (void)s; return "idle";
}
void nh_provisioner_status_set_state(nh_provisioner_status_writer *w,
                                     nh_provisioner_state s) {
    (void)w; (void)s;
}
void nh_provisioner_status_set_last_provisioned_ts(
    nh_provisioner_status_writer *w, int64_t epoch_secs) {
    (void)w; (void)epoch_secs;
}
void nh_provisioner_status_set_last_error_class(
    nh_provisioner_status_writer *w, const char *class_slug) {
    (void)w; (void)class_slug;
}
void nh_provisioner_status_set_chunks(nh_provisioner_status_writer *w,
                                      uint32_t done, uint32_t pending) {
    (void)w; (void)done; (void)pending;
}
int nh_provisioner_status_emit(nh_provisioner_status_writer *w,
                               const char *homedir,
                               uid_t uid, gid_t gid,
                               const char *homedir_override) {
    (void)w; (void)homedir; (void)uid; (void)gid; (void)homedir_override;
    return 0;
}
void nh_provisioner_notify(nh_provisioner_status_writer *w,
                           const char *event_slug,
                           const char *summary,
                           const char *body) {
    (void)w; (void)event_slug; (void)summary; (void)body;
}

#endif /* NH_AUTH_BROKER_ENABLE_PORTHOME */
