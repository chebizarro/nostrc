/* nss-widgets.c — see nss-ui.h.
 * SPDX-License-Identifier: MIT
 */
#include "nss-ui.h"
#include "nss-config.h"
#include "nss-identity.h"
#include "nss-lists.h"
#include "nss-net.h"

NssContext *
nss_context_new(AdwPreferencesWindow *window)
{
  NssContext *ctx = g_new0(NssContext, 1);
  g_atomic_ref_count_init(&ctx->ref);
  ctx->window = window;
  g_object_add_weak_pointer(G_OBJECT(window), (gpointer *)&ctx->window);
  g_autoptr(GError) err = NULL;
  ctx->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
  if (ctx->bus == NULL)
    g_warning("no session bus: %s", err->message);
  ctx->soup = soup_session_new_with_options("timeout", 10, "user-agent",
                                            "org.nostr.Settings/" NSS_VERSION, NULL);
  return ctx;
}

NssContext *
nss_context_ref(NssContext *ctx)
{
  g_atomic_ref_count_inc(&ctx->ref);
  return ctx;
}

void
nss_context_unref(NssContext *ctx)
{
  if (ctx == NULL || !g_atomic_ref_count_dec(&ctx->ref))
    return;
  if (ctx->window)
    g_object_remove_weak_pointer(G_OBJECT(ctx->window), (gpointer *)&ctx->window);
  g_clear_object(&ctx->bus);
  g_clear_object(&ctx->soup);
  g_free(ctx);
}

void
nss_toast(NssContext *ctx, const gchar *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  g_autofree gchar *msg = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  if (ctx->window == NULL)
    return;
  AdwToast *t = adw_toast_new(msg);
  adw_toast_set_use_markup(t, FALSE);
  adw_toast_set_timeout(t, 5);
  adw_preferences_window_add_toast(ctx->window, t);
}

typedef struct {
  GWeakRef       owner;
  NssWorkFunc    func;
  NssDoneFunc    done;
  gpointer       data;
  GDestroyNotify data_free;
  GDestroyNotify result_free;
} Job;

static void
job_free(gpointer p)
{
  Job *j = p;
  g_weak_ref_clear(&j->owner);
  if (j->data_free && j->data)
    j->data_free(j->data);
  g_free(j);
}

static void
job_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
  (void)src; (void)c;
  Job *j = data;
  GError *err = NULL;
  gpointer r = j->func(j->data, &err);
  if (err != NULL)
    g_task_return_error(task, err);
  else
    g_task_return_pointer(task, r, j->result_free);
}

static void
job_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
  (void)src; (void)user_data;
  Job *j = g_task_get_task_data(G_TASK(res));
  GError *err = NULL;
  gpointer r = g_task_propagate_pointer(G_TASK(res), &err);
  g_autoptr(GtkWidget) owner = g_weak_ref_get(&j->owner);
  if (owner != NULL && j->done)
    j->done(owner, r, err, j->data);
  if (r && j->result_free)
    j->result_free(r);
  g_clear_error(&err);
}

void
nss_run(GtkWidget *owner, NssWorkFunc func, NssDoneFunc done, gpointer data,
        GDestroyNotify data_free, GDestroyNotify result_free)
{
  Job *j = g_new0(Job, 1);
  g_weak_ref_init(&j->owner, owner);
  j->func = func;
  j->done = done;
  j->data = data;
  j->data_free = data_free;
  j->result_free = result_free;
  GTask *task = g_task_new(NULL, NULL, job_done, NULL);
  g_task_set_task_data(task, j, job_free);
  g_task_run_in_thread(task, job_thread);
  g_object_unref(task);
}

GtkWidget *
nss_info_row(const gchar *title, const gchar *subtitle)
{
  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  if (subtitle)
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
  adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), TRUE);
  return row;
}

void
nss_row_set_subtitle_plain(GtkWidget *row, const gchar *subtitle)
{
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle ? subtitle : "");
}

GtkWidget *
nss_status_row(const gchar *title, const gchar *subtitle, const gchar *icon)
{
  GtkWidget *row = nss_info_row(title, subtitle);
  if (icon) {
    GtkWidget *img = gtk_image_new_from_icon_name(icon);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), img);
  }
  return row;
}

#define DYN_KEY "nss-dynamic-rows"

void
nss_group_add_dynamic(AdwPreferencesGroup *g, GtkWidget *row)
{
  GPtrArray *rows = g_object_get_data(G_OBJECT(g), DYN_KEY);
  if (rows == NULL) {
    rows = g_ptr_array_new();
    g_object_set_data_full(G_OBJECT(g), DYN_KEY, rows, (GDestroyNotify)g_ptr_array_unref);
  }
  g_ptr_array_add(rows, row);
  adw_preferences_group_add(g, row);
}

void
nss_group_clear_dynamic(AdwPreferencesGroup *g)
{
  GPtrArray *rows = g_object_get_data(G_OBJECT(g), DYN_KEY);
  for (guint i = 0; rows && i < rows->len; i++)
    adw_preferences_group_remove(g, g_ptr_array_index(rows, i));
  if (rows)
    g_ptr_array_set_size(rows, 0);
}

GtkWidget *
nss_suffix_button(const gchar *icon, const gchar *tooltip)
{
  GtkWidget *b = gtk_button_new_from_icon_name(icon);
  gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(b, "flat");
  gtk_widget_set_tooltip_text(b, tooltip);
  gtk_accessible_update_property(GTK_ACCESSIBLE(b), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 tooltip, -1);
  return b;
}

gboolean
nss_desktop_installed(const gchar *desktop_id)
{
#ifdef NSS_HAVE_DESKTOP_APPINFO
  g_autoptr(GDesktopAppInfo) info = g_desktop_app_info_new(desktop_id);
  return info != NULL;
#else
  (void)desktop_id;
  return FALSE;
#endif
}

void
nss_launch_desktop(NssContext *ctx, const gchar *desktop_id, const gchar *action)
{
#ifndef NSS_HAVE_DESKTOP_APPINFO
  (void)action;
  nss_toast(ctx, "%s is not installed", desktop_id);
#else
  g_autoptr(GDesktopAppInfo) info = g_desktop_app_info_new(desktop_id);
  if (info == NULL) {
    nss_toast(ctx, "%s is not installed", desktop_id);
    return;
  }
  GdkDisplay *d = gtk_widget_get_display(GTK_WIDGET(ctx->window));
  g_autoptr(GdkAppLaunchContext) lc = gdk_display_get_app_launch_context(d);
  if (action != NULL) {
    g_desktop_app_info_launch_action(info, action, G_APP_LAUNCH_CONTEXT(lc));
    return;
  }
  g_autoptr(GError) err = NULL;
  if (!g_app_info_launch(G_APP_INFO(info), NULL, G_APP_LAUNCH_CONTEXT(lc), &err))
    nss_toast(ctx, "Could not start %s: %s", desktop_id, err->message);
#endif
}

/* ── user lists ────────────────────────────────────────────────────────── */

#define FETCH_TIMEOUT_MS 6000

void
nss_user_lists_free(NssUserLists *l)
{
  if (l == NULL)
    return;
  g_free(l->pubkey_hex);
  g_strfreev(l->signer_relays);
  if (l->relay_list)
    g_ptr_array_unref(l->relay_list);
  g_free(l->relay_list_source);
  g_strfreev(l->blossom);
  g_free(l->blossom_source);
  g_free(l);
}

NssUserLists *
nss_user_lists_load(GDBusConnection *bus, gboolean want_blossom, GError **error)
{
  g_autofree gchar *npub = nss_signer_get_npub(bus, error);
  if (npub == NULL)
    return NULL;
  guint8 pk[32];
  if (!nss_parse_pubkey(npub, pk, error))
    return NULL;
  NssUserLists *l = g_new0(NssUserLists, 1);
  l->pubkey_hex = nss_pubkey_to_hex(pk);
  GError *e = NULL;
  l->signer_relays = nss_signer_get_relays(bus, &e);
  if (l->signer_relays == NULL) {
    g_debug("GetRelays: %s", e ? e->message : "?");
    g_clear_error(&e);
    l->signer_relays = g_new0(gchar *, 1);
  }
  NssNet net;
  nss_net_init(&net);
  const gchar *session[] = { net.session_socket ? NSS_SESSION_RELAY_URL : NULL, NULL };
  g_auto(GStrv) disc = nss_strv_union(session, (const gchar *const *)l->signer_relays, NULL);
  g_autofree gchar *ev = nss_net_fetch_replaceable(&net, (const gchar *const *)disc, 10002,
                                                   l->pubkey_hex, FETCH_TIMEOUT_MS,
                                                   &l->relay_list_source);
  if (ev)
    l->relay_list = nss_relay_list_parse(ev, NULL);
  if (want_blossom) {
    g_auto(GStrv) w = nss_relay_list_urls(l->relay_list, TRUE);
    g_auto(GStrv) disc2 = nss_strv_union((const gchar *const *)disc, (const gchar *const *)w, NULL);
    g_autofree gchar *bev = nss_net_fetch_replaceable(&net, (const gchar *const *)disc2, 10063,
                                                      l->pubkey_hex, FETCH_TIMEOUT_MS,
                                                      &l->blossom_source);
    if (bev)
      l->blossom = nss_blossom_list_parse(bev, NULL);
  }
  nss_net_clear(&net);
  return l;
}

void
nss_publish_job_free(gpointer p)
{
  NssPublishJob *j = p;
  g_clear_object(&j->bus);
  g_free(j->unsigned_json);
  g_strfreev(j->targets);
  g_strfreev(j->required);
  g_free(j);
}

void
nss_publish_report_free(gpointer p)
{
  NssPublishReport *r = p;
  if (r == NULL)
    return;
  nss_publish_report_clear(r);
  g_free(r);
}

gpointer
nss_publish_job_run(gpointer p, GError **error)
{
  NssPublishJob *j = p;
  g_autoptr(NostrPublishSigner) signer = nss_signer_connect(j->bus, NULL, error);
  if (signer == NULL)
    return NULL;
  NssNet net;
  nss_net_init(&net);
  NssPublishReport *r = g_new0(NssPublishReport, 1);
  GError *local = NULL;
  gboolean ok = nss_net_publish(&net, signer, j->unsigned_json,
                                (const gchar *const *)j->targets,
                                (const gchar *const *)j->required, 15, r, &local);
  nss_net_clear(&net);
  /* A partial publish still returns the per-relay report: the page derives
   * success from n_required_ok == n_required. Only a failure before any
   * relay was tried (signing refused, no targets) is an error. */
  if (!ok && (r->results == NULL || r->results->len == 0)) {
    nss_publish_report_free(r);
    g_propagate_error(error, local);
    return NULL;
  }
  g_clear_error(&local);
  return r;
}

gchar *
nss_publish_report_text(const gpointer p)
{
  const NssPublishReport *r = p;
  GString *s = g_string_new(NULL);
  for (guint i = 0; r && r->results && i < r->results->len; i++) {
    NssRelayResult *x = g_ptr_array_index(r->results, i);
    g_string_append_printf(s, "%s%s: %s", i ? "\n" : "", x->url, x->detail);
  }
  return g_string_free(s, FALSE);
}
