/*
 * notify_subs.c — subscription driver implementation.
 *
 * Threading model:
 *   - One "connector" thread per home_relay, spawned by
 *     nostr_notify_subs_start(). Each connects with libnostr's
 *     `nostr_relay_new()` + `nostr_relay_connect()`, fires two subs (kinds
 *     9-12 and kind 1059 with `#p`), then loops on `nostr_subscription_
 *     get_events_channel()` to receive events.
 *   - The connector thread is the only place that touches its NostrRelay
 *     handle. On EVENT it captures the fields the main thread needs
 *     (id, kind, first h-tag) into a small POD, snapshots the current
 *     generation, and posts a g_main_context_invoke_full() callback that
 *     runs on the main thread's context.
 *   - The main thread's callback re-checks the generation (mismatch →
 *     drop), coalesces per (kind, thread-key), then builds and dispatches
 *     the GNotification.
 *
 * Suppression contract (§3.3 D5 revised, Finding 12):
 *   - Every callback that touches user-visible state re-checks the
 *     generation at every yield boundary. If `nsn_guard_check()` returns
 *     false, drop silently.
 */
#include "notify_subs.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <glib.h>

/* libnostr client API. Headers are in libnostr/include; the daemon links
 * against `nostr` (the client lib) built by libnostr's CMakeLists. */
#include "channel.h"
#include "context.h"
#include "error.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-relay.h"
#include "nostr-subscription.h"
#include "nostr-tag.h"

#include "notify_gnotification.h"

/*
 * Coalescing catalog — maps a withdraw-id (thread key) to a coalesce count
 * so the "N new messages" style summary is possible in a follow-up. For v1
 * we simply reuse the withdraw_id so successive events in the same thread
 * replace the visible notification.
 *
 * Lifetime note: allocated on subs_start; never freed. `subs_stop` clears
 * `app` to NULL so any dispatch callbacks still queued on the default
 * GMainContext when the connectors are joined will early-exit on the NULL
 * app check rather than dereference a freed struct (Oracle review Q4 UAF).
 * A restart reuses the same dispatcher, rebinding `app` and clearing
 * `live_ids`.
 */
typedef struct {
  GApplication *app;               /* NULL after subs_stop */
  NostrNotifySuppressGuard *guard; /* borrowed */
  GHashTable *live_ids;
  GMutex ids_mu;
} NotifyDispatcher;

/*
 * Serialize cursor updates + persisted saves across connector threads
 * (Oracle review Q5). Without this, two connectors racing on
 * `ctx->last_seen_created_at` produce a lost-max data race (UB per C11),
 * and two concurrent `cursor_save_locked` calls share the same
 * `<path>.tmp` file so both truncate + write into the same inode before
 * either renames.
 */
static GMutex g_cursor_mu;
static gboolean g_cursor_mu_inited = FALSE;

/*
 * Per-connector state.
 */
typedef struct {
  NostrNotifySubsCtx *ctx;
  char url[512];
  NostrRelay *relay;
  NostrSubscription *sub_groups;    /* kinds 9-12 */
  NostrSubscription *sub_dms;       /* kind 1059 with #p */
  pthread_t thread;
  _Atomic int stop;
  NotifyDispatcher *dispatcher;
} SubsConnector;

/*
 * Cross-thread event payload posted to the main context. All strings owned;
 * freed after dispatch.
 */
typedef struct {
  NotifyDispatcher *dispatcher;
  uint64_t generation_snapshot;
  int kind;
  char *event_id_hex; /* lowercase 64-char */
  char *h_tag;        /* NIP-29 only; NULL for DMs */
  char *content;      /* NIP-29 only; NULL for DMs */
} DispatchedEvent;

static void dispatched_event_free(gpointer p) {
  DispatchedEvent *e = p;
  if (!e) return;
  g_free(e->event_id_hex);
  g_free(e->h_tag);
  g_free(e->content);
  g_free(e);
}

/* ------------------------------------------------------------------- */
/* Cursor persistence                                                  */
/* ------------------------------------------------------------------- */

static void cursor_path(char *out, size_t out_sz) {
  const char *xstate = getenv("XDG_STATE_HOME");
  const char *home = getenv("HOME");
  if (xstate && xstate[0] == '/') {
    snprintf(out, out_sz, "%s/nostr-notify/cursor", xstate);
  } else if (home && home[0] == '/') {
    snprintf(out, out_sz, "%s/.local/state/nostr-notify/cursor", home);
  } else {
    out[0] = '\0';
  }
}

static void cursor_load(NostrNotifySubsCtx *ctx) {
  char path[512]; cursor_path(path, sizeof path);
  if (!path[0]) return;
  FILE *f = fopen(path, "r");
  if (!f) return;
  long long v = 0;
  if (fscanf(f, "%lld", &v) == 1 && v > 0)
    ctx->last_seen_created_at = (int64_t)v;
  fclose(f);
}

/*
 * Persist the cursor. MUST be called with g_cursor_mu held (the name says
 * "locked" and this fn enforces the contract via a debug-only check that
 * expands away in release builds — g_mutex_trylock returns TRUE if the
 * lock is free, i.e. NOT held).
 */
static void cursor_save_locked(NostrNotifySubsCtx *ctx) {
  char path[512]; cursor_path(path, sizeof path);
  if (!path[0]) return;
  /* Ensure parent dir exists 0700. */
  char *slash = strrchr(path, '/');
  if (slash) {
    *slash = '\0';
    /* mkdir -p style, minimal */
    char buf[512]; snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++) {
      if (*p == '/') {
        *p = '\0';
        if (mkdir(buf, 0700) != 0 && errno != EEXIST) { /* best-effort */ }
        *p = '/';
      }
    }
    if (mkdir(buf, 0700) != 0 && errno != EEXIST) { /* best-effort */ }
    *slash = '/';
  }
  char tmp[520]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return;
  char line[64];
  int n = snprintf(line, sizeof line, "%lld\n",
                   (long long)ctx->last_seen_created_at);
  if (n > 0) {
    ssize_t written = 0;
    while (written < n) {
      ssize_t w = write(fd, line + written, (size_t)(n - written));
      if (w <= 0) { close(fd); unlink(tmp); return; }
      written += w;
    }
  }
  fsync(fd);
  close(fd);
  if (rename(tmp, path) != 0) unlink(tmp);
}

/* ------------------------------------------------------------------- */
/* Main-thread dispatch                                                */
/* ------------------------------------------------------------------- */

static gboolean dispatch_on_main(gpointer user_data) {
  DispatchedEvent *ev = user_data;
  if (!ev || !ev->dispatcher || !ev->dispatcher->app) return G_SOURCE_REMOVE;

  /* Re-check the generation on the main thread. This is the last chance
   * before we side-effect. */
  if (!nsn_guard_check(ev->dispatcher->guard, ev->generation_snapshot))
    return G_SOURCE_REMOVE;

  NostrNotifyBuild build; memset(&build, 0, sizeof build);
  GNotification *n = NULL;

  if (ev->kind == 1059) {
    n = nostr_notify_build_dm(ev->event_id_hex, &build);
  } else {
    /* NIP-29 preview. group_display_name is looked up from the daemon's
     * metadata cache — for v1 the cache is trivial and returns NULL, so
     * the h_tag itself is the visible title (still markup-safe). */
    n = nostr_notify_build_group(NULL, ev->h_tag, ev->event_id_hex,
                                 ev->content, &build);
  }

  if (!n || !build.withdraw_id) {
    nostr_notify_build_dispose(&build);
    if (n) g_object_unref(n);
    return G_SOURCE_REMOVE;
  }

  /* One more re-check right before send. On a single-threaded GMainContext
   * (Type=SERVICE + default context) the name-owner-changed callback
   * cannot run between check #1 and check #2 unless the builder yields
   * back to the loop — but a future change adding a sync D-Bus lookup or
   * nested main-context iteration inside the builders would break that,
   * so the second check is load-bearing. Keep the builders pure. */
  if (nsn_guard_check(ev->dispatcher->guard, ev->generation_snapshot)) {
    g_application_send_notification(ev->dispatcher->app, build.withdraw_id, n);

    g_mutex_lock(&ev->dispatcher->ids_mu);
    g_hash_table_replace(ev->dispatcher->live_ids,
                         g_strdup(build.withdraw_id),
                         GUINT_TO_POINTER(1));
    g_mutex_unlock(&ev->dispatcher->ids_mu);
  }

  nostr_notify_build_dispose(&build);
  g_object_unref(n);
  return G_SOURCE_REMOVE;
}

/*
 * Withdraw every currently-live notification id. Called from the main
 * thread when GNostr claims its bus name (suppression edge).
 */
void nostr_notify_withdraw_all(GApplication *app, NotifyDispatcher *disp) {
  if (!app || !disp) return;
  g_mutex_lock(&disp->ids_mu);
  GHashTableIter it;
  gpointer key, value;
  (void)value;
  g_hash_table_iter_init(&it, disp->live_ids);
  while (g_hash_table_iter_next(&it, &key, &value)) {
    g_application_withdraw_notification(app, (const char *)key);
  }
  g_hash_table_remove_all(disp->live_ids);
  g_mutex_unlock(&disp->ids_mu);
}

/* ------------------------------------------------------------------- */
/* Event extraction on the connector thread                            */
/* ------------------------------------------------------------------- */

/*
 * Extract an "h" tag value from an event. Returns a newly-allocated string
 * (caller frees) or NULL if no h tag is present.
 */
static char *event_first_h_tag(NostrEvent *ev) {
  if (!ev) return NULL;
  void *tags_opaque = nostr_event_get_tags(ev);
  if (!tags_opaque) return NULL;
  NostrTags *tags = (NostrTags *)tags_opaque;
  size_t n = nostr_tags_size(tags);
  for (size_t i = 0; i < n; i++) {
    NostrTag *t = nostr_tags_get(tags, i);
    if (!t) continue;
    const char *k = nostr_tag_get_key(t);
    if (k && strcmp(k, "h") == 0) {
      const char *v = nostr_tag_get_value(t);
      if (v && *v) return g_strdup(v);
    }
  }
  return NULL;
}

static bool hex64_ok(const char *s) {
  if (!s) return false;
  size_t n = strlen(s);
  if (n != 64) return false;
  for (size_t i = 0; i < 64; i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

/*
 * Connector thread: pull events off the subscription channels and post to
 * the main context.
 */
static void *connector_thread(void *arg) {
  SubsConnector *c = (SubsConnector *)arg;
  NostrNotifySubsCtx *ctx = c->ctx;

  Error *err = NULL;
  c->relay = nostr_relay_new(NULL, c->url, &err);
  if (!c->relay) {
    fprintf(stderr, "nostr-notify: relay_new(%s): %s\n", c->url,
            (err && err->message) ? err->message : "?");
    if (err) free_error(err);
    return NULL;
  }
  if (!nostr_relay_connect(c->relay, &err)) {
    fprintf(stderr, "nostr-notify: connect(%s): %s\n", c->url,
            (err && err->message) ? err->message : "?");
    if (err) free_error(err);
    return NULL;
  }

  /* Filter 1: NIP-29 kinds 9-12. Server-side h-tag filter would require
   * the daemon's cached group set; v1 subscribes broadly and drops on
   * the client side (matching the plan text's "group set discovered from
   * cached 39000-39003 metadata — minimal in-daemon cache"). */
  NostrFilters *f_groups = nostr_filters_new();
  {
    NostrFilter *f = nostr_filter_new();
    const int kinds[] = {9, 10, 11, 12};
    nostr_filter_set_kinds(f, kinds, sizeof kinds / sizeof *kinds);
    nostr_filters_add(f_groups, f);
  }
  c->sub_groups = nostr_subscription_new(c->relay, f_groups);

  /* Filter 2: NIP-17 kind 1059 with #p = user_pubkey. */
  NostrFilters *f_dms = nostr_filters_new();
  {
    NostrFilter *f = nostr_filter_new();
    int kind = 1059;
    nostr_filter_set_kinds(f, &kind, 1);
    /* Tag filter: {"#p":[user_pubkey_hex]} */
    NostrTag *ptag = nostr_tag_new("p", ctx->user_pubkey_hex, NULL);
    NostrTags *tags = nostr_tags_new(1, ptag);
    nostr_filter_set_tags(f, tags);
    nostr_filters_add(f_dms, f);
  }
  c->sub_dms = nostr_subscription_new(c->relay, f_dms);

  if (!nostr_subscription_fire(c->sub_groups, &err)) {
    fprintf(stderr, "nostr-notify: sub_groups.fire: %s\n",
            (err && err->message) ? err->message : "?");
    if (err) { free_error(err); err = NULL; }
  }
  if (!nostr_subscription_fire(c->sub_dms, &err)) {
    fprintf(stderr, "nostr-notify: sub_dms.fire: %s\n",
            (err && err->message) ? err->message : "?");
    if (err) { free_error(err); err = NULL; }
  }

  /* Drain both subscription channels alternately until we're told to
   * stop. Use short-timeout receives so we can poll for stop and back
   * off on failure. */
  GoChannel *ev_groups = nostr_subscription_get_events_channel(c->sub_groups);
  GoChannel *ev_dms    = nostr_subscription_get_events_channel(c->sub_dms);

  while (!atomic_load(&c->stop)) {
    for (int which = 0; which < 2; which++) {
      GoChannel *chan = which ? ev_dms : ev_groups;
      if (!chan) continue;
      void *out = NULL;
      /* Non-blocking receive: rely on libnostr's channel semantics — this
       * call blocks until an event OR the channel closes. Because we
       * spawn one thread per connector, blocking here is fine. */
      if (go_channel_receive(chan, &out) != 0 || !out) continue;
      NostrEvent *ev = (NostrEvent *)out;

      int kind = nostr_event_get_kind(ev);
      char *eid = nostr_event_get_id(ev);
      const char *content = nostr_event_get_content(ev);
      int64_t created_at = nostr_event_get_created_at(ev);

      /* Catch-up guard: skip events older than the persisted cursor OR
       * older than daemon_start - 10min, whichever is stricter. Prevents
       * a notification flood on relay reconnect. */
      int64_t oldest_ok = ctx->daemon_start_unix - 600;
      if (ctx->last_seen_created_at > oldest_ok)
        oldest_ok = ctx->last_seen_created_at;
      if (created_at < oldest_ok) {
        free(eid);
        /* We do not touch ev — libnostr owns it via the channel. */
        continue;
      }

      /* Only forward valid hex event ids. Something rejected here is
       * malformed and should not surface as an obscure notification. */
      if (!hex64_ok(eid)) { free(eid); continue; }

      char *h_tag = NULL;
      if (kind == 9 || kind == 10 || kind == 11 || kind == 12) {
        h_tag = event_first_h_tag(ev);
        /* Untagged kind-9/10/11/12 events are not in any group — drop. */
        if (!h_tag) { free(eid); continue; }
      }

      /* Snapshot the generation on the connector thread. The main-thread
       * dispatch will re-check on delivery. This is the "at every yield
       * boundary" contract from the plan. */
      uint64_t gen = nsn_guard_current(c->dispatcher->guard);
      if (!nsn_guard_check(c->dispatcher->guard, gen)) {
        free(eid); g_free(h_tag); continue;
      }

      DispatchedEvent *dev = g_new0(DispatchedEvent, 1);
      dev->dispatcher = c->dispatcher;
      dev->generation_snapshot = gen;
      dev->kind = kind;
      dev->event_id_hex = g_strdup(eid);
      dev->h_tag = h_tag; /* transferred */
      dev->content = (kind == 1059) ? NULL : g_strdup(content ? content : "");
      free(eid);

      /* Advance the cursor before dispatching so a crash mid-send
       * doesn't reflood on restart. Serialized across connectors so the
       * max-update + persist is atomic (Oracle review Q5); without this
       * two connectors racing on ctx->last_seen_created_at are UB per
       * C11 and their cursor.tmp files race on the same inode. */
      g_mutex_lock(&g_cursor_mu);
      if (created_at > ctx->last_seen_created_at) {
        ctx->last_seen_created_at = created_at;
        cursor_save_locked(ctx);
      }
      g_mutex_unlock(&g_cursor_mu);

      /* Post to the main context. */
      g_main_context_invoke_full(NULL, G_PRIORITY_DEFAULT_IDLE,
                                 dispatch_on_main, dev,
                                 dispatched_event_free);
    }
  }

  /* Teardown. Close subs, then relay. */
  if (c->sub_groups) {
    nostr_subscription_close(c->sub_groups, NULL);
    nostr_subscription_free(c->sub_groups);
    c->sub_groups = NULL;
  }
  if (c->sub_dms) {
    nostr_subscription_close(c->sub_dms, NULL);
    nostr_subscription_free(c->sub_dms);
    c->sub_dms = NULL;
  }
  if (c->relay) {
    nostr_relay_disconnect(c->relay);
    nostr_relay_unref(c->relay);
    c->relay = NULL;
  }
  return NULL;
}

/* ------------------------------------------------------------------- */
/* Public API                                                          */
/* ------------------------------------------------------------------- */

/*
 * A single opaque dispatcher owned by the daemon main. `subs.c` internal
 * holder; declared in the .c so headers stay ABI-light.
 */
static NotifyDispatcher *g_dispatcher = NULL;
static SubsConnector **g_connectors = NULL;
static size_t g_connectors_n = 0;

bool nostr_notify_subs_start(NostrNotifySubsCtx *ctx) {
  if (!ctx || !ctx->app || !ctx->guard) return false;
  if (!ctx->user_pubkey_hex[0]) {
    fprintf(stderr,
            "nostr-notify: no pubkey (signer idle); daemon will not "
            "subscribe.\n");
    return false;
  }

  if (ctx->mode == NSN_UPSTREAM_SESSION_RELAY) {
    fprintf(stderr,
            "nostr-notify: session_relay mode requires a Unix-socket ws "
            "client (libnostr today speaks ws://+wss:// only); falling "
            "back to home_relays direct.\n");
    ctx->mode = NSN_UPSTREAM_DIRECT;
  }

  if (ctx->home_relays_count == 0) {
    fprintf(stderr,
            "nostr-notify: no home_relays configured; daemon will not "
            "subscribe.\n");
    return false;
  }

  cursor_load(ctx);
  ctx->daemon_start_unix = (int64_t)time(NULL);

  if (!g_cursor_mu_inited) {
    g_mutex_init(&g_cursor_mu);
    g_cursor_mu_inited = TRUE;
  }

  /* Reuse an existing dispatcher on restart — freeing it would break any
   * dispatch_on_main callbacks still queued from the previous run. */
  if (!g_dispatcher) {
    g_dispatcher = g_new0(NotifyDispatcher, 1);
    g_dispatcher->live_ids = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                   g_free, NULL);
    g_mutex_init(&g_dispatcher->ids_mu);
  }
  g_dispatcher->app = ctx->app;
  g_dispatcher->guard = ctx->guard;

  g_connectors_n = ctx->home_relays_count;
  g_connectors = g_new0(SubsConnector *, g_connectors_n);
  for (size_t i = 0; i < g_connectors_n; i++) {
    SubsConnector *c = g_new0(SubsConnector, 1);
    c->ctx = ctx;
    c->dispatcher = g_dispatcher;
    snprintf(c->url, sizeof c->url, "%s", ctx->home_relays[i]);
    if (pthread_create(&c->thread, NULL, connector_thread, c) != 0) {
      fprintf(stderr, "nostr-notify: pthread_create(%s): %s\n", c->url,
              strerror(errno));
      g_free(c);
      continue;
    }
    g_connectors[i] = c;
  }
  return true;
}

void nostr_notify_subs_stop(NostrNotifySubsCtx *ctx) {
  (void)ctx;
  if (!g_connectors) return;
  /* Signal each connector to stop, then join. Closing the subs from the
   * connector thread avoids racing with libnostr's callback machinery. */
  for (size_t i = 0; i < g_connectors_n; i++) {
    if (g_connectors[i]) atomic_store(&g_connectors[i]->stop, 1);
  }
  /* Nudge the connectors by disconnecting the relays — this unblocks any
   * in-flight go_channel_receive by closing the channel. Done from THIS
   * thread deliberately: nostr_relay_disconnect() is documented as
   * thread-safe. */
  for (size_t i = 0; i < g_connectors_n; i++) {
    if (g_connectors[i] && g_connectors[i]->relay)
      nostr_relay_disconnect(g_connectors[i]->relay);
  }
  for (size_t i = 0; i < g_connectors_n; i++) {
    if (!g_connectors[i]) continue;
    pthread_join(g_connectors[i]->thread, NULL);
    g_free(g_connectors[i]);
  }
  g_free(g_connectors);
  g_connectors = NULL;
  g_connectors_n = 0;

  if (g_dispatcher) {
    /* Withdraw first so the shell state matches the stopped daemon. */
    nostr_notify_withdraw_all(g_dispatcher->app, g_dispatcher);
    /* Invalidate — do NOT free. Any dispatch_on_main callbacks still queued
     * on the default GMainContext (posted by the connector threads before
     * they were joined) may still dereference `ev->dispatcher`. Freeing
     * the struct here is a use-after-free (Oracle review Q4). Instead we
     * clear `app` so `dispatch_on_main`'s early-return on `!dispatcher->app`
     * drops any stragglers safely. The dispatcher is intentionally leaked
     * across the process lifetime — a small, bounded cost. */
    g_dispatcher->app = NULL;
  }
}

/*
 * Exposed for the daemon's suppression edge handler: withdraw all live
 * notifications when GNostr claims its bus name. Called from the main
 * thread. Safe to call when no dispatcher is set (no-op).
 */
void nostr_notify_subs_withdraw_all(void);
void nostr_notify_subs_withdraw_all(void) {
  if (g_dispatcher && g_dispatcher->app)
    nostr_notify_withdraw_all(g_dispatcher->app, g_dispatcher);
}
