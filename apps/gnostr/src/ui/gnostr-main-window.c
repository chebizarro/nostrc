#include "gnostr-main-window.h"
#include "gnostr-composer.h"
#include "gnostr-timeline-view.h"
#include "../ipc/signer_ipc.h"
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <time.h>

/* Implement as-if SimplePool is fully functional; guarded to avoid breaking builds until wired. */
#ifdef GNOSTR_ENABLE_REAL_SIMPLEPOOL
#include "nostr-simple-pool.h"
#include "nostr-relay.h"
#include "nostr-subscription.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-json.h"
#include "channel.h"
#include "error.h"
#include "context.h"
#endif

#define UI_RESOURCE "/org/gnostr/ui/ui/gnostr-main-window.ui"

struct _GnostrMainWindow {
  GtkApplicationWindow parent_instance;
  // Template children
  GtkWidget *stack;
  GtkWidget *timeline;
  GWeakRef timeline_ref; /* weak ref to avoid UAF in async */
  GtkWidget *btn_settings;
  GtkWidget *btn_menu;
  GtkWidget *composer;
  GtkWidget *btn_refresh;
  GtkWidget *toast_revealer;
  GtkWidget *toast_label;
  /* Session state */
  GHashTable *seen_texts; /* owned; keys are g_strdup(text), values unused */
  GHashTable *seen_ids;   /* owned; keys are g_strdup(event id hex), values unused */
};

G_DEFINE_TYPE(GnostrMainWindow, gnostr_main_window, GTK_TYPE_APPLICATION_WINDOW)

static void gnostr_main_window_dispose(GObject *obj) {
  /* Place for future object unrefs; template children are owned by hierarchy. */
  G_OBJECT_CLASS(gnostr_main_window_parent_class)->dispose(obj);
}

static void gnostr_main_window_finalize(GObject *obj) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(obj);
  if (self->seen_texts) {
    g_hash_table_destroy(self->seen_texts);
    self->seen_texts = NULL;
  }
  if (self->seen_ids) {
    g_hash_table_destroy(self->seen_ids);
    self->seen_ids = NULL;
  }
  g_weak_ref_clear(&self->timeline_ref);
  gtk_widget_dispose_template(GTK_WIDGET(obj), GNOSTR_TYPE_MAIN_WINDOW);
  G_OBJECT_CLASS(gnostr_main_window_parent_class)->finalize(obj);
}

static void on_settings_clicked(GnostrMainWindow *self, GtkButton *button) {
  (void)self; (void)button;
  g_message("settings clicked");
}

static void toast_hide_cb(gpointer data) {
  GnostrMainWindow *win = GNOSTR_MAIN_WINDOW(data);
  if (win && win->toast_revealer)
    gtk_revealer_set_reveal_child(GTK_REVEALER(win->toast_revealer), FALSE);
}

static void show_toast(GnostrMainWindow *self, const char *msg) {
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));
  if (!self->toast_revealer || !self->toast_label) return;
  gtk_label_set_text(GTK_LABEL(self->toast_label), msg ? msg : "");
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->toast_revealer), TRUE);
  /* Auto-hide after 2.5s */
  g_timeout_add_once(2500, (GSourceOnceFunc)toast_hide_cb, self);
}

/* Forward declaration to satisfy initial_refresh */
static void timeline_refresh_async(GnostrMainWindow *self, int limit);
static gboolean initial_refresh_cb(gpointer data);

/* Trampoline with correct type for g_timeout_add_once */
static void initial_refresh_timeout_cb(gpointer data) {
  (void)initial_refresh_cb(data);
}

static gboolean initial_refresh_cb(gpointer data) {
  GnostrMainWindow *win = GNOSTR_MAIN_WINDOW(data);
  if (!win || !GTK_IS_WIDGET(win->timeline)) {
    g_message("initial refresh skipped: timeline not ready");
    return G_SOURCE_REMOVE;
  }
  g_message("initial refresh: starting timeline refresh");
  timeline_refresh_async(win, 5);
  return G_SOURCE_REMOVE;
}

typedef struct {
  GnostrMainWindow *self; /* strong ref, owned */
  int limit;
} RefreshTaskData;

typedef struct {
  GnostrMainWindow *self; /* strong ref */
  GPtrArray *lines;       /* transfer full */
} IdleApplyCtx;

static void idle_apply_ctx_free(IdleApplyCtx *c) {
  if (!c) return;
  if (c->lines) {
    g_debug("idle free: unref lines ptr=%p", (void*)c->lines);
    g_ptr_array_unref(c->lines);
  }
  if (c->self) {
    g_debug("idle free: unref self ptr=%p", (void*)c->self);
    g_object_unref(c->self);
  }
  g_free(c);
}

/* Forward declare idle applier for trampoline */
static gboolean apply_timeline_lines_idle(gpointer user_data);

/* Trampoline to safely reschedule apply on a timeout with correct callback type */
static void apply_timeline_lines_timeout_cb(gpointer user_data) {
  (void)apply_timeline_lines_idle(user_data);
}

static gboolean apply_timeline_lines_idle(gpointer user_data) {
  IdleApplyCtx *c = (IdleApplyCtx*)user_data;
  GnostrMainWindow *self = c ? c->self : NULL;
  GPtrArray *lines = c ? c->lines : NULL;
  g_message("apply_timeline_lines_idle: entry (self=%p, lines=%p)", (void*)self, (void*)lines);
  if (!self || !GNOSTR_IS_MAIN_WINDOW(self) || !lines) {
    g_debug("apply_timeline_lines_idle: missing self or lines (self=%p lines=%p)", (void*)self, (void*)lines);
    idle_apply_ctx_free(c);
    return G_SOURCE_REMOVE;
  }
  GtkWidget *timeline = g_weak_ref_get(&self->timeline_ref);
  if (!timeline || !GTK_IS_WIDGET(timeline) || !gtk_widget_get_root(timeline)) {
    /* Widget not yet in a realized hierarchy; retry shortly instead of dropping */
    g_debug("timeline not ready (tl=%p); will retry apply of %u lines", (void*)timeline, lines->len);
    g_timeout_add_once(100, (GSourceOnceFunc)apply_timeline_lines_timeout_cb, c);
    if (timeline) g_object_unref(timeline);
    return G_SOURCE_REMOVE;
  }
  if (GNOSTR_IS_TIMELINE_VIEW(timeline)) {
    g_message("apply_timeline_lines_idle: applying %u lines to timeline=%p (type=%s)",
              lines->len, (void*)timeline, G_OBJECT_TYPE_NAME(timeline));
    const gboolean skip_dedup = g_getenv("GNOSTR_SKIP_DEDUP") != NULL;
    if (skip_dedup) g_warning("GNOSTR_SKIP_DEDUP is set: bypassing deduplication");
    guint applied = 0, skipped = 0;
    for (guint i = 0; i < lines->len; i++) {
      const char *raw = (const char*)g_ptr_array_index(lines, i);
      const char *tab = raw ? strchr(raw, '\t') : NULL;
      const char *id = NULL;
      const char *text = raw;
      if (tab) {
        /* Expect format: "<id>\t<text>" */
        id = raw;
        text = tab + 1;
      }
      /* Initialize sets on demand */
      if (!self->seen_ids) self->seen_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
      if (!self->seen_texts) self->seen_texts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

      gboolean allow = TRUE;
      if (skip_dedup) {
        allow = TRUE;
      } else {
      if (id && g_str_has_prefix(id, "-") == FALSE && strlen(id) == 64) {
        if (g_hash_table_contains(self->seen_ids, id)) {
          allow = FALSE;
          g_debug("apply: skip duplicate id %.12s...", id);
        } else {
          g_hash_table_add(self->seen_ids, g_strndup(id, 64));
        }
      } else {
        /* Fallback dedup by text if no id */
        if (g_hash_table_contains(self->seen_texts, text)) {
          allow = FALSE;
          g_debug("apply: skip duplicate text %.32s", text);
        } else {
          g_hash_table_add(self->seen_texts, g_strdup(text));
        }
      } }
      if (allow) {
        g_debug("apply: prepend text=%.60s", text ? text : "");
        gnostr_timeline_view_prepend_text(GNOSTR_TIMELINE_VIEW(timeline), text);
        applied++;
      } else {
        skipped++;
      }
    }
    g_message("apply_timeline_lines_idle: done applying (applied=%u skipped=%u)", applied, skipped);
  } else {
    g_warning("timeline widget is not GnostrTimelineView: type=%s", G_OBJECT_TYPE_NAME(timeline));
  }
  g_object_unref(timeline);
  idle_apply_ctx_free(c);
  g_message("apply_timeline_lines_idle: exit");
  return G_SOURCE_REMOVE;
}

static void refresh_task_data_free(RefreshTaskData *d) {
  if (!d) return;
  if (d->self) g_object_unref(d->self);
  g_free(d);
}

/* Worker: for now, synthesize placeholder lines. Later, query relays via libnostr. */
static void timeline_refresh_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable) {
  (void)source; (void)cancellable;
  RefreshTaskData *d = (RefreshTaskData*)task_data;
  GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
  g_message("timeline_refresh_worker: start (limit=%d)", d ? d->limit : -1);

#ifdef GNOSTR_ENABLE_REAL_SIMPLEPOOL
  /* Real path: connect to each relay, subscribe, drain events until EOSE/CLOSED, format lines. */
  const char *urls[] = {
    "wss://relay.damus.io",
    "wss://nos.lol"
  };
  size_t url_count = sizeof(urls)/sizeof(urls[0]);

  for (size_t i = 0; i < url_count; ++i) {
    const char *url = urls[i];
    g_message("worker: connecting %s", url);
    Error *err = NULL;
    NostrRelay *relay = nostr_relay_new(NULL, url, &err);
    if (!relay) {
      g_ptr_array_add(lines, g_strdup_printf("-\t[error] relay_new %s: %s", url, err ? err->message : "unknown"));
      if (err) free_error(err);
      continue;
    }
    if (!nostr_relay_connect(relay, &err)) {
      g_ptr_array_add(lines, g_strdup_printf("-\t[error] connect %s: %s", url, err ? err->message : "unknown"));
      if (err) free_error(err);
      nostr_relay_free(relay);
      continue;
    }

    /* Build filters per-relay to avoid shared ownership pitfalls */
    /* Build filters: kind=1 notes with a sensible window and limit */
    NostrFilter *f = nostr_filter_new();
    int kinds[] = { 1 };
    nostr_filter_set_kinds(f, kinds, 1);
    int lim = d && d->limit > 0 ? d->limit : 20;
    nostr_filter_set_limit(f, lim);
    time_t now = time(NULL);
    int64_t since = (int64_t)now - 3600; /* last 1h */
    if (since < 0) since = 0;
    nostr_filter_set_since_i64(f, since);
    NostrFilters *fs = nostr_filters_new();
    nostr_filters_add(fs, f);

    GoContext *bg = go_context_background();
    NostrSubscription *sub = (NostrSubscription*)nostr_relay_prepare_subscription(relay, bg, fs);
    if (!sub) {
      g_ptr_array_add(lines, g_strdup_printf("-\t[error] prepare sub %s", url));
      nostr_relay_disconnect(relay);
      nostr_relay_free(relay);
      nostr_filters_free(fs);
      continue;
    }
    if (!nostr_subscription_fire(sub, &err)) {
      g_ptr_array_add(lines, g_strdup_printf("-\t[error] fire sub %s: %s", url, err ? err->message : "unknown"));
      if (err) free_error(err);
      nostr_subscription_close(sub, NULL);
      nostr_subscription_free(sub);
      nostr_relay_disconnect(relay);
      nostr_relay_free(relay);
      nostr_filters_free(fs);
      continue;
    }

    GoChannel *ch_events = nostr_subscription_get_events_channel(sub);
    GoChannel *ch_eose   = nostr_subscription_get_eose_channel(sub);
    GoChannel *ch_closed = nostr_subscription_get_closed_channel(sub);

    /* Drain events; on EOSE, keep only a short grace period to catch stragglers. */
    gboolean eose_seen = FALSE;
    gint64 eose_time_us = 0;
    int collected = 0; /* per-relay count */
    const gint64 started_us = g_get_monotonic_time();
    const gint64 per_relay_hard_us = started_us + (3 * G_TIME_SPAN_SECOND);
    /* If busy, extend briefly when events arrive, but cap to 300ms after last event post-EOSE */
    gint64 quiet_deadline_us = g_get_monotonic_time() + (300 * G_TIME_SPAN_MILLISECOND);
    while (TRUE) {
      /* CLOSED wins: exit immediately */
      void *data = NULL;
      if (ch_closed && go_channel_try_receive(ch_closed, &data) == 0) {
        const char *reason = (const char *)data;
        g_ptr_array_add(lines, g_strdup_printf("%s\t[%s] CLOSED: %s", "-", url, reason ? reason : ""));
        break;
      }

      /* Drain as many events as are immediately available */
      gboolean any_event = FALSE;
      while (ch_events && go_channel_try_receive(ch_events, &data) == 0) {
        any_event = TRUE;
        NostrEvent *evt = (NostrEvent *)data;
        const char *pubkey = nostr_event_get_pubkey(evt);
        const char *content = nostr_event_get_content(evt);
        int64_t ts = nostr_event_get_created_at(evt);
        if (!content) content = "";
        /* Ensure valid UTF-8 to prevent GTK crashes */
        gchar *one = g_utf8_make_valid(content, -1);
        /* Truncate to first line */
        gchar *nl = one ? strchr(one, '\n') : NULL;
        if (nl) *nl = '\0';
        /* Limit display length to 160 UTF-8 chars */
        if (one && g_utf8_strlen(one, -1) > 160) {
          gchar *tmp = g_utf8_substring(one, 0, 160);
          g_free(one);
          one = tmp;
        }
        const char *eid = nostr_event_get_id(evt);
        if (!eid) eid = "-";
        gchar *row = g_strdup_printf("%s\t[%s] %s | %s (%ld)", eid, url, pubkey ? pubkey : "(anon)", one ? one : "", (long)ts);
        g_debug("worker: row eid=%.12s... text=%.40s", eid, one ? one : "");
        g_ptr_array_add(lines, row);
        collected++;
        if (one) g_free(one);
        if (collected >= lim) {
          g_debug("worker: %s reached limit=%d; breaking", url, lim);
          any_event = FALSE; /* prevent extending deadlines */
          break;
        }
      }
      if (any_event) {
        /* Extend quiet deadline a bit after receiving events to allow small bursts */
        quiet_deadline_us = g_get_monotonic_time() + (150 * G_TIME_SPAN_MILLISECOND);
      }

      /* Observe EOSE but don't break until grace period elapses */
      data = NULL;
      if (!eose_seen && ch_eose && go_channel_try_receive(ch_eose, &data) == 0) {
        eose_seen = TRUE;
        eose_time_us = g_get_monotonic_time();
        g_debug("worker: %s EOSE seen", url);
      }

      /* Exit conditions */
      const gint64 now_us = g_get_monotonic_time();
      if (collected >= lim) {
        break;
      }
      if (eose_seen) {
        /* After EOSE, exit after quiet period of ~150ms without new events */
        if (!any_event && now_us > quiet_deadline_us) break;
        /* Hard-cap maximum wait after EOSE to 500ms */
        if (eose_time_us && now_us - eose_time_us > (500 * G_TIME_SPAN_MILLISECOND)) break;
      }
      /* Also hard-cap per relay duration to avoid indefinite loops on busy relays */
      if (now_us > per_relay_hard_us) {
        g_debug("worker: %s hard timeout reached (%.2fs)", url, (per_relay_hard_us - started_us) / 1000000.0);
        break;
      }

      /* if nothing urgent, yield briefly */
      g_usleep(1000 * 5); /* 5ms */
    }

    g_message("worker: %s collected=%d (limit=%d)", url, collected, lim);
    /* Cleanup per relay */
    nostr_subscription_close(sub, NULL);
    nostr_subscription_free(sub);
    nostr_relay_disconnect(relay);
    nostr_relay_free(relay);
    nostr_filters_free(fs);
  }
#else
  /* Fallback demo: synthesize placeholder lines. */
  time_t now = time(NULL);
  int n = d ? d->limit : 5;
  for (int i = 0; i < n; i++) {
    gchar *s = g_strdup_printf("-\t[demo] note %d at %ld", i+1, (long)now);
    g_ptr_array_add(lines, s);
  }
#endif

  /* Post UI apply directly to the main loop to ensure timely update */
  if (d && d->self) {
    IdleApplyCtx *c = g_new0(IdleApplyCtx, 1);
    c->self = g_object_ref(d->self);
    c->lines = lines; /* transfer ownership to idle ctx */
    g_message("timeline_refresh_worker: scheduling main-loop apply (lines=%u)", lines ? lines->len : 0);
    g_main_context_invoke_full(NULL, G_PRIORITY_DEFAULT, (GSourceFunc)apply_timeline_lines_idle, c, NULL);
  } else {
    /* No window; just free lines */
    g_ptr_array_unref(lines);
  }
  g_task_return_boolean(task, TRUE);
  g_message("timeline_refresh_worker: done");
}

static void timeline_refresh_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  GTask *task = G_TASK(res);
  RefreshTaskData *d = (RefreshTaskData*)g_task_get_task_data(task);
  GError *error = NULL;
  gboolean ok = g_task_propagate_boolean(task, &error);
  /* Get window from our task data (strong ref we manage) */
  GnostrMainWindow *self = d ? d->self : NULL;
  if (!self || !GNOSTR_IS_MAIN_WINDOW(self)) {
    g_warning("timeline_refresh_complete: self missing or invalid");
    return;
  }
  /* Re-enable button (guard against stale/destroyed widget) */
  if (self->btn_refresh && G_IS_OBJECT(self->btn_refresh) && GTK_IS_WIDGET(self->btn_refresh))
    gtk_widget_set_sensitive(self->btn_refresh, TRUE);
  if (!ok || error) {
    show_toast(self, error && error->message ? error->message : "Failed to load timeline");
    g_clear_error(&error);
  } else {
    g_message("timeline_refresh_complete: worker signaled ok");
    show_toast(self, "Timeline updated");
  }
  /* Do not unref self: owned by application; GTask manages a temporary ref. */
}

static void timeline_refresh_async(GnostrMainWindow *self, int limit) {
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));
  if (self->btn_refresh) gtk_widget_set_sensitive(self->btn_refresh, FALSE);
  show_toast(self, "Refreshing timeline...");
  /* Create task without a source object; we manage a strong ref in task data */
  GTask *task = g_task_new(NULL, NULL, timeline_refresh_complete, NULL);
  RefreshTaskData *d = g_new0(RefreshTaskData, 1);
  d->self = g_object_ref(self);
  d->limit = limit;
  g_task_set_task_data(task, d, (GDestroyNotify)refresh_task_data_free);
  g_task_run_in_thread(task, timeline_refresh_worker);
  g_object_unref(task);
}

static void on_refresh_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  timeline_refresh_async(self, 10);
}

/* No proxy signals: org.nostr.Signer.xml does not define approval signals. */

typedef struct {
  NostrSignerProxy *proxy; /* not owned */
  char *event_json;        /* owned */
  char *current_user;      /* owned */
  char *app_id;            /* owned */
} PostCtx;

static void post_ctx_free(PostCtx *ctx){
  if (!ctx) return;
  g_free(ctx->event_json);
  g_free(ctx->current_user);
  g_free(ctx->app_id);
  g_free(ctx);
}

static void on_sign_event_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  NostrSignerProxy *proxy = (NostrSignerProxy*)source;
  GError *error = NULL;
  char *signature = NULL;
  gboolean ok = nostr_org_nostr_signer_call_sign_event_finish(proxy, &signature, res, &error);
  if (!ok) {
    const gchar *remote = error ? g_dbus_error_get_remote_error(error) : NULL;
    g_warning("SignEvent async failed: %s%s%s",
              error ? error->message : "unknown error",
              remote ? " (remote=" : "",
              remote ? remote : "");
    if (remote) g_dbus_error_strip_remote_error(error);
    g_clear_error(&error);
    /* UI feedback */
    if (GTK_IS_WINDOW(self)) {
      GtkAlertDialog *dlg = gtk_alert_dialog_new("Signing failed");
      gtk_alert_dialog_set_detail(dlg, "The signer could not sign your event. Check your identity and permissions.");
      gtk_alert_dialog_show(dlg, GTK_WINDOW(self));
      g_object_unref(dlg);
    }
    PostCtx *ctx = (PostCtx*)g_object_steal_data(G_OBJECT(self), "postctx-temp");
    post_ctx_free(ctx);
    return;
  }
  g_message("signature: %s", signature ? signature : "<null>");
  g_free(signature);
  PostCtx *ctx = (PostCtx*)g_object_steal_data(G_OBJECT(self), "postctx-temp");
  post_ctx_free(ctx);
}

static void on_get_pubkey_done(GObject *source, GAsyncResult *res, gpointer user_data){
  NostrSignerProxy *proxy = (NostrSignerProxy*)source;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  PostCtx *ctx = (PostCtx*)g_object_get_data(G_OBJECT(self), "postctx-temp");
  GError *error = NULL;
  char *npub = NULL;
  gboolean ok = nostr_org_nostr_signer_call_get_public_key_finish(proxy, &npub, res, &error);
  if (!ok) {
    const gchar *remote = error ? g_dbus_error_get_remote_error(error) : NULL;
    g_warning("GetPublicKey failed: %s%s%s",
              error ? error->message : "unknown error",
              remote ? " (remote=" : "",
              remote ? remote : "");
    if (remote) g_dbus_error_strip_remote_error(error);
    g_clear_error(&error);
    if (GTK_IS_WINDOW(self)) {
      GtkAlertDialog *dlg = gtk_alert_dialog_new("Identity unavailable");
      gtk_alert_dialog_set_detail(dlg, "No signing identity is configured. Import or select an identity in GNostr Signer.");
      gtk_alert_dialog_show(dlg, GTK_WINDOW(self));
      g_object_unref(dlg);
    }
    post_ctx_free(ctx);
    return;
  }
  g_message("using identity npub=%s", npub ? npub : "<null>");
  /* Ensure SignEvent uses the same identity returned here */
  g_clear_pointer(&ctx->current_user, g_free);
  ctx->current_user = g_strdup(npub ? npub : "");
  g_free(npub);
  /* Proceed to SignEvent */
  g_message("calling SignEvent (async)... json-len=%zu", strlen(ctx->event_json));
  nostr_org_nostr_signer_call_sign_event(
      proxy,
      ctx->event_json,
      ctx->current_user,
      ctx->app_id,
      NULL,
      on_sign_event_done,
      self);
}

static void on_composer_post_requested(GnostrComposer *composer, const char *text, gpointer user_data) {
  g_message("on_composer_post_requested enter composer=%p user_data=%p", (void*)composer, user_data);
  if (!GNOSTR_IS_COMPOSER(composer)) { g_warning("composer instance invalid"); return; }
  if (!GNOSTR_IS_MAIN_WINDOW(user_data)) { g_warning("main window user_data invalid"); return; }
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!text || !*text) {
    g_message("empty post ignored");
    return;
  }
  g_message("post-requested: %s", text);

  // Build minimal Nostr event JSON (kind=1) for signing
  time_t now = time(NULL);
  gchar *escaped = g_strescape(text, NULL);
  gchar *event_json = g_strdup_printf("{\"kind\":1,\"created_at\":%ld,\"tags\":[],\"content\":\"%s\"}", (long)now, escaped ? escaped : "");
  g_free(escaped);

  GError *error = NULL;
  g_message("acquiring signer proxy...");
  NostrSignerProxy *proxy = gnostr_signer_proxy_get(&error);
  if (!proxy) {
    g_warning("Signer proxy unavailable: %s", error ? error->message : "unknown error");
    g_clear_error(&error);
    g_free(event_json);
    return;
  }

  // Ensure the service is actually present on the bus; otherwise avoid long timeouts
  const gchar *owner = g_dbus_proxy_get_name_owner(G_DBUS_PROXY(proxy));
  if (!owner || !*owner) {
    g_warning("Signer service is not running (no name owner). Start gnostr-signer-daemon and retry.");
    g_free(event_json);
    return;
  }

  // For SignEvent, allow sufficient time for user approval in the signer UI
  g_dbus_proxy_set_default_timeout(G_DBUS_PROXY(proxy), 600000); // 10 minutes

  PostCtx *ctx = g_new0(PostCtx, 1);
  ctx->proxy = proxy;
  ctx->event_json = event_json; /* take ownership */
  ctx->current_user = g_strdup(""); // will be set to npub after GetPublicKey
  ctx->app_id = g_strdup("org.gnostr.Client");
  g_object_set_data_full(G_OBJECT(self), "postctx-temp", ctx, (GDestroyNotify)post_ctx_free);

  /* Pre-check identity exists and is configured */
  g_message("calling GetPublicKey (async) to verify identity...");
  nostr_org_nostr_signer_call_get_public_key(
      proxy,
      NULL,
      on_get_pubkey_done,
      self);

  // TODO: assemble full event (id/sig/pubkey), publish to relay, push into timeline
}

static void gnostr_main_window_class_init(GnostrMainWindowClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *gobj_class = G_OBJECT_CLASS(klass);
  gobj_class->dispose = gnostr_main_window_dispose;
  gobj_class->finalize = gnostr_main_window_finalize;
  /* Ensure custom widget types are registered before template parsing */
  g_type_ensure(GNOSTR_TYPE_TIMELINE_VIEW);
  g_type_ensure(GNOSTR_TYPE_COMPOSER);
  gtk_widget_class_set_template_from_resource(widget_class, UI_RESOURCE);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, stack);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, timeline);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, btn_settings);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, btn_menu);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, composer);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, btn_refresh);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, toast_revealer);
  gtk_widget_class_bind_template_child(widget_class, GnostrMainWindow, toast_label);
  gtk_widget_class_bind_template_callback(widget_class, on_settings_clicked);
}

static void gnostr_main_window_init(GnostrMainWindow *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  g_return_if_fail(self->composer != NULL);
  /* Initialize weak refs to template children needed in async paths */
  g_weak_ref_init(&self->timeline_ref, self->timeline);
  /* Initialize dedup table */
  self->seen_texts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  /* Build app menu for header button */
  if (self->btn_menu) {
    GMenu *menu = g_menu_new();
    g_menu_append(menu, "Quit", "app.quit");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(self->btn_menu), G_MENU_MODEL(menu));
    g_object_unref(menu);
  }
  g_message("connecting post-requested handler on composer=%p", (void*)self->composer);
  g_signal_connect(self->composer, "post-requested",
                   G_CALLBACK(on_composer_post_requested), self);
  if (self->btn_refresh) {
    g_signal_connect(self->btn_refresh, "clicked", G_CALLBACK(on_refresh_clicked), self);
  }
  /* Ensure Timeline page is visible initially */
  if (self->stack && self->timeline && GTK_IS_STACK(self->stack)) {
    gtk_stack_set_visible_child(GTK_STACK(self->stack), self->timeline);
  }
  /* Seed one row to validate the view wiring */
  if (self->timeline && GNOSTR_IS_TIMELINE_VIEW(self->timeline)) {
    g_message("seeding demo row into timeline view");
    gnostr_timeline_view_prepend_text(GNOSTR_TIMELINE_VIEW(self->timeline), "[demo] GNostr timeline ready");
  } else {
    g_warning("timeline is not a GnostrTimelineView at init: type=%s", self->timeline ? G_OBJECT_TYPE_NAME(self->timeline) : "(null)");
  }
  /* Seed initial items so Timeline page isn't empty */
  g_timeout_add_once(150, (GSourceOnceFunc)initial_refresh_timeout_cb, self);
}

GnostrMainWindow *gnostr_main_window_new(GtkApplication *app) {
  return g_object_new(GNOSTR_TYPE_MAIN_WINDOW, "application", app, NULL);
}
