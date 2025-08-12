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
#endif

#define UI_RESOURCE "/org/gnostr/ui/ui/gnostr-main-window.ui"

struct _GnostrMainWindow {
  GtkApplicationWindow parent_instance;
  // Template children
  GtkWidget *stack;
  GtkWidget *timeline;
  GtkWidget *btn_settings;
  GtkWidget *btn_menu;
  GtkWidget *composer;
  GtkWidget *btn_refresh;
  GtkWidget *toast_revealer;
  GtkWidget *toast_label;
};

G_DEFINE_TYPE(GnostrMainWindow, gnostr_main_window, GTK_TYPE_APPLICATION_WINDOW)

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

/* Forward declaration to satisfy initial_refresh_cb */
static void timeline_refresh_async(GnostrMainWindow *self, int limit);

static gboolean initial_refresh_cb(gpointer data) {
  GnostrMainWindow *win = GNOSTR_MAIN_WINDOW(data);
  if (!win || !GTK_IS_WIDGET(win->timeline)) {
    g_message("initial refresh skipped: timeline not ready");
    return G_SOURCE_REMOVE;
  }
  timeline_refresh_async(win, 5);
  return G_SOURCE_REMOVE;
}

typedef struct {
  int limit;
} RefreshTaskData;

static void refresh_task_data_free(RefreshTaskData *d) {
  if (!d) return;
  g_free(d);
}

/* Worker: for now, synthesize placeholder lines. Later, query relays via libnostr. */
static void timeline_refresh_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable) {
  (void)source; (void)cancellable;
  RefreshTaskData *d = (RefreshTaskData*)task_data;
  GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);

#ifdef GNOSTR_ENABLE_REAL_SIMPLEPOOL
  /* Real path: connect to each relay, subscribe, drain events until EOSE/CLOSED, format lines. */
  const char *urls[] = {
    "wss://relay.damus.io",
    "wss://nos.lol"
  };
  size_t url_count = sizeof(urls)/sizeof(urls[0]);

  /* Build filters: kind=1 notes. TODO: set since/limit based on RefreshTaskData */
  NostrFilter *f = nostr_filter_new();
  int kinds[] = { 1 };
  nostr_filter_set_kinds(f, kinds, 1);
  NostrFilters *fs = nostr_filters_new();
  nostr_filters_add(fs, f);

  for (size_t i = 0; i < url_count; ++i) {
    const char *url = urls[i];
    Error *err = NULL;
    NostrRelay *relay = nostr_relay_new(NULL, url, &err);
    if (!relay) {
      g_ptr_array_add(lines, g_strdup_printf("[error] relay_new %s: %s", url, err ? err->message : "unknown"));
      if (err) error_free(err);
      continue;
    }
    if (!nostr_relay_connect(relay, &err)) {
      g_ptr_array_add(lines, g_strdup_printf("[error] connect %s: %s", url, err ? err->message : "unknown"));
      if (err) error_free(err);
      nostr_relay_free(relay);
      continue;
    }

    NostrSubscription *sub = (NostrSubscription*)nostr_relay_prepare_subscription(relay, NULL, fs);
    if (!sub) {
      g_ptr_array_add(lines, g_strdup_printf("[error] prepare sub %s", url));
      nostr_relay_disconnect(relay);
      nostr_relay_free(relay);
      continue;
    }
    if (!nostr_subscription_fire(sub, &err)) {
      g_ptr_array_add(lines, g_strdup_printf("[error] fire sub %s: %s", url, err ? err->message : "unknown"));
      if (err) error_free(err);
      nostr_subscription_close(sub, NULL);
      nostr_subscription_free(sub);
      nostr_relay_disconnect(relay);
      nostr_relay_free(relay);
      continue;
    }

    GoChannel *ch_events = nostr_subscription_get_events_channel(sub);
    GoChannel *ch_eose   = nostr_subscription_get_eose_channel(sub);
    GoChannel *ch_closed = nostr_subscription_get_closed_channel(sub);

    int got_eose = 0;
    while (!got_eose) {
      /* Prioritize CLOSED, then EOSE, then events. Use try-receive to avoid blocking deadlocks. */
      void *data = NULL;
      if (ch_closed && go_channel_try_receive(ch_closed, &data) == 0) {
        const char *reason = (const char *)data;
        g_ptr_array_add(lines, g_strdup_printf("[%s] CLOSED: %s", url, reason ? reason : ""));
        break;
      }
      data = NULL;
      if (ch_eose && go_channel_try_receive(ch_eose, &data) == 0) {
        /* EOSE observed for this relay */
        got_eose = 1;
        continue;
      }
      data = NULL;
      if (ch_events && go_channel_try_receive(ch_events, &data) == 0) {
        NostrEvent *evt = (NostrEvent *)data;
        const char *pubkey = nostr_event_get_pubkey(evt);
        const char *content = nostr_event_get_content(evt);
        int64_t ts = nostr_event_get_created_at(evt);
        if (!content) content = "";
        /* Truncate content to one line, 160 chars */
        gchar *one = g_strdup(content);
        gchar *nl = one ? strchr(one, '\n') : NULL;
        if (nl) *nl = '\0';
        if (one && g_utf8_strlen(one, -1) > 160) {
          gchar *tmp = g_utf8_substring(one, 0, 160);
          g_free(one);
          one = tmp;
        }
        gchar *row = g_strdup_printf("[%s] %s | %s (%ld)", url, pubkey ? pubkey : "(anon)", one ? one : "", (long)ts);
        g_ptr_array_add(lines, row);
        if (one) g_free(one);
        /* Event memory ownership: assuming channel provides owned pointer and relay/sub frees after consumption; if not, add nostr_event_free(evt) here. */
        continue;
      }

      /* If nothing available, sleep briefly to yield */
      g_usleep(1000 * 5); /* 5ms */
    }

    /* Cleanup per relay */
    nostr_subscription_close(sub, NULL);
    nostr_subscription_free(sub);
    nostr_relay_disconnect(relay);
    nostr_relay_free(relay);
  }

  nostr_filters_free(fs);
#else
  /* Fallback demo: synthesize placeholder lines. */
  time_t now = time(NULL);
  int n = d ? d->limit : 5;
  for (int i = 0; i < n; i++) {
    gchar *s = g_strdup_printf("[demo] note %d at %ld", i+1, (long)now);
    g_ptr_array_add(lines, s);
  }
#endif

  g_task_return_pointer(task, lines, (GDestroyNotify)g_ptr_array_unref);
}

static void timeline_refresh_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  GTask *task = G_TASK(res);
  RefreshTaskData *d = (RefreshTaskData*)g_task_get_task_data(task);
  GError *error = NULL;
  GPtrArray *lines = g_task_propagate_pointer(task, &error);
  /* Use the task source object; GTask keeps it alive during callback. */
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(g_task_get_source_object(task));
  if (!self || !GTK_IS_APPLICATION_WINDOW(self)) {
    if (lines) g_ptr_array_unref(lines);
    return;
  }
  /* Re-enable button */
  if (self->btn_refresh) gtk_widget_set_sensitive(self->btn_refresh, TRUE);
  if (error) {
    show_toast(self, error->message ? error->message : "Failed to load timeline");
    g_clear_error(&error);
  } else if (lines) {
    if (!self->timeline || !GTK_IS_WIDGET(self->timeline)) {
      g_warning("timeline widget not ready; dropping %u lines", lines->len);
    } else {
      for (guint i = 0; i < lines->len; i++) {
        const char *text = (const char*)g_ptr_array_index(lines, i);
        if (GNOSTR_IS_TIMELINE_VIEW(self->timeline))
          gnostr_timeline_view_prepend_text(GNOSTR_TIMELINE_VIEW(self->timeline), text);
      }
    }
    g_ptr_array_unref(lines);
    show_toast(self, "Timeline updated");
  } else {
    show_toast(self, "Failed to load timeline");
  }
  /* Do not unref self: owned by application; GTask manages a temporary ref. */
}

static void timeline_refresh_async(GnostrMainWindow *self, int limit) {
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));
  if (self->btn_refresh) gtk_widget_set_sensitive(self->btn_refresh, FALSE);
  show_toast(self, "Refreshing timeline...");
  GTask *task = g_task_new(self, NULL, timeline_refresh_complete, NULL);
  RefreshTaskData *d = g_new0(RefreshTaskData, 1);
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
  /* Seed initial items so Timeline page isn't empty; temporarily disabled while isolating crash */
  // g_timeout_add_once(1, (GSourceOnceFunc)initial_refresh_cb, self);
}

GnostrMainWindow *gnostr_main_window_new(GtkApplication *app) {
  return g_object_new(GNOSTR_TYPE_MAIN_WINDOW, "application", app, NULL);
}
