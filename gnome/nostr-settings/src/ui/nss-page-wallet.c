/* nss-page-wallet.c — org.nostr.Wallet1 pairing, per-app read and receive
 * access and daily budgets.
 * SPDX-License-Identifier: MIT
 *
 * Pair / Unpair are always confirmed by the agent's own dialog; this page
 * never sees the pairing secret. As the agent's trusted settings app (see
 * nss-wallet.h) the page lists apps with ListApps and changes read access
 * (SetReadAccess) and receive access (SetReceiveAccess) directly; budget changes (SetBudget) are confirmed by the
 * agent's dialog. When the agent does not trust this process the page falls
 * back to budgets.json and the agent confirms every change. Grants, budgets
 * and the GNOME Shell row are rendered as AdwExpanderRows; the page
 * re-lists on the agent's AppsChanged signal.
 *
 * Keep it that way: no GAction (reachable over the app's D-Bus surface)
 * may change wallet grants — only direct widget handlers.
 */
#include "nss-ui.h"
#include "nss-wallet.h"

typedef struct {
  NssContext          *ctx;
  GtkWidget           *page;
  GtkWidget           *status;
  GtkWidget           *unpair;
  GtkWidget           *auth_row;   /* "Connect with your wallet app" */
  GtkWidget           *wait_row;   /* pending request: link, Copy, Cancel */
  gchar               *auth_uri;
  guint                auth_sub;
  AdwEntryRow         *pair_entry;
  AdwPreferencesGroup *budgets;
  guint                props_sub;
  guint                apps_sub;
  gboolean             busy;
  gboolean             trusted;   /* last ListApps succeeded */
  GHashTable          *expanded;  /* app ids whose row is open (kept across re-lists) */
} Page;

static void refresh(Page *p);

static void
page_free(gpointer data)
{
  Page *p = data;
  if (p->props_sub && p->ctx->bus)
    g_dbus_connection_signal_unsubscribe(p->ctx->bus, p->props_sub);
  if (p->apps_sub && p->ctx->bus)
    g_dbus_connection_signal_unsubscribe(p->ctx->bus, p->apps_sub);
  if (p->auth_sub && p->ctx->bus)
    g_dbus_connection_signal_unsubscribe(p->ctx->bus, p->auth_sub);
  g_free(p->auth_uri);
  g_hash_table_unref(p->expanded);
  nss_context_unref(p->ctx);
  g_free(p);
}

/* ── state ── */

typedef struct {
  gboolean   available;
  gchar     *error;
  gboolean   paired;
  gchar     *lud16;
  gchar     *wallet_pubkey;
  gchar    **relays;
  GPtrArray *budgets;
  gchar     *budgets_error;
  gboolean   trusted;        /* budgets came from ListApps */
  gchar     *untrusted_why;  /* why not (ListApps error), for the group note */
} State;

static void
state_free(gpointer p)
{
  State *s = p;
  g_free(s->error);
  g_free(s->lud16);
  g_free(s->wallet_pubkey);
  g_strfreev(s->relays);
  if (s->budgets)
    g_ptr_array_unref(s->budgets);
  g_free(s->budgets_error);
  g_free(s->untrusted_why);
  g_free(s);
}

static gpointer
load(gpointer data, GError **error)
{
  (void)error;
  GDBusConnection *bus = data;
  State *s = g_new0(State, 1);
  GError *e = NULL;
  /* GetAll auto-starts the (D-Bus activatable) agent: harmless, it idles. */
  g_autoptr(GVariant) r = g_dbus_connection_call_sync(
    bus, NSS_WALLET_BUS_NAME, NSS_WALLET_OBJ_PATH, "org.freedesktop.DBus.Properties",
    "GetAll", g_variant_new("(s)", NSS_WALLET_IFACE), G_VARIANT_TYPE("(a{sv})"),
    G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &e);
  if (r == NULL) {
    g_dbus_error_strip_remote_error(e);
    s->error = g_strdup(e->message);
    g_clear_error(&e);
  } else {
    s->available = TRUE;
    g_autoptr(GVariant) d = g_variant_get_child_value(r, 0);
    (void)g_variant_lookup(d, "Paired", "b", &s->paired);
    (void)g_variant_lookup(d, "Lud16", "s", &s->lud16);
    (void)g_variant_lookup(d, "WalletPubkey", "s", &s->wallet_pubkey);
    (void)g_variant_lookup(d, "Relays", "^as", &s->relays);
    g_autoptr(GVariant) apps = g_dbus_connection_call_sync(
      bus, NSS_WALLET_BUS_NAME, NSS_WALLET_OBJ_PATH, NSS_WALLET_IFACE, "ListApps", NULL,
      G_VARIANT_TYPE("(a{sa{sv}})"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &e);
    if (apps != NULL) {
      g_autoptr(GVariant) body = g_variant_get_child_value(apps, 0);
      s->budgets = nss_wallet_apps_from_variant(body);
      s->trusted = TRUE;
      return s;
    }
    g_dbus_error_strip_remote_error(e);
    s->untrusted_why = g_strdup(e->message);
    g_clear_error(&e);
  }
  g_autofree gchar *path = nss_wallet_budgets_path();
  s->budgets = nss_wallet_budgets_load(path, NULL, &e);
  if (s->budgets == NULL) {
    s->budgets_error = g_strdup(e ? e->message : "unreadable");
    g_clear_error(&e);
  }
  return s;
}

/* ── apps: read access, receive access + budget ── */

typedef struct {
  Page       *p;
  gchar      *app_id;
  guint64     current_msat;
  GtkWidget  *apply;
  AdwSpinRow *spin;
} AppRow;

static void
app_row_free(gpointer d)
{
  AppRow *b = d;
  g_free(b->app_id);
  g_free(b);
}

static void
on_budget_value(AdwSpinRow *row, GParamSpec *ps, gpointer data)
{
  (void)ps;
  AppRow *b = data;
  guint64 msat = (guint64)adw_spin_row_get_value(row) * 1000;
  gtk_widget_set_visible(b->apply, msat != b->current_msat);
}

typedef struct {
  GtkWidget *page;
  gchar     *ok_msg;
  gchar     *fail_prefix;
} AgentCall;

static void
agent_call_done(GObject *src, GAsyncResult *res, gpointer data)
{
  AgentCall *ac = data;
  Page *p = g_object_get_data(G_OBJECT(ac->page), "nss-page");
  g_autoptr(GError) err = NULL;
  g_autoptr(GVariant) r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
  if (p) {
    if (r == NULL) {
      g_dbus_error_strip_remote_error(err);
      nss_toast(p->ctx, "%s: %s", ac->fail_prefix, err->message);
    } else if (ac->ok_msg) {
      nss_toast(p->ctx, "%s", ac->ok_msg);
    }
    refresh(p); /* also reverts a switch the agent refused */
  }
  g_object_unref(ac->page);
  g_free(ac->ok_msg);
  g_free(ac->fail_prefix);
  g_free(ac);
}

static void
call_app_method(Page *p, const gchar *method, GVariant *params, gboolean agent_confirms,
                const gchar *ok, const gchar *fail)
{
  if (agent_confirms || !p->trusted)
    nss_toast(p->ctx, "Confirm the change in the wallet dialog");
  AgentCall *ac = g_new0(AgentCall, 1);
  ac->page = g_object_ref(p->page);
  ac->ok_msg = g_strdup(ok);
  ac->fail_prefix = g_strdup(fail);
  g_dbus_connection_call(p->ctx->bus, NSS_WALLET_BUS_NAME, NSS_WALLET_OBJ_PATH, NSS_WALLET_IFACE,
                         method, params, NULL, G_DBUS_CALL_FLAGS_NONE,
                         NSS_WALLET_PROMPT_TIMEOUT_MS, NULL, agent_call_done, ac);
}

static void
on_budget_apply(GtkButton *btn, gpointer data)
{
  (void)btn;
  AppRow *b = data;
  guint64 sats = (guint64)adw_spin_row_get_value(b->spin);
  guint32 msat = (guint32)MIN(sats * 1000, (guint64)G_MAXUINT32);
  gtk_widget_set_sensitive(b->apply, FALSE);
  /* Lowering one's own budget is the only unconfirmed case, and Settings
   * never changes its own. */
  call_app_method(b->p, "SetBudget", g_variant_new("(su)", b->app_id, msat), TRUE,
                  "Budget updated", "Budget not changed");
}

static void
on_read_toggled(AdwSwitchRow *row, GParamSpec *ps, gpointer data)
{
  (void)ps;
  AppRow *b = data;
  gboolean allow = adw_switch_row_get_active(row);
  gtk_widget_set_sensitive(GTK_WIDGET(row), FALSE);
  call_app_method(b->p, "SetReadAccess", g_variant_new("(sb)", b->app_id, allow), FALSE,
                  allow ? "Access granted" : "Access revoked",
                  allow ? "Access not granted" : "Access not revoked");
}

static void
on_receive_toggled(AdwSwitchRow *row, GParamSpec *ps, gpointer data)
{
  (void)ps;
  AppRow *b = data;
  gboolean allow = adw_switch_row_get_active(row);
  gtk_widget_set_sensitive(GTK_WIDGET(row), FALSE);
  call_app_method(b->p, "SetReceiveAccess", g_variant_new("(sb)", b->app_id, allow), FALSE,
                  allow ? "Invoices allowed" : "Invoices revoked",
                  allow ? "Invoices not allowed" : "Invoices not revoked");
}

static void
on_expanded(AdwExpanderRow *row, GParamSpec *ps, gpointer data)
{
  (void)ps;
  AppRow *b = data;
  if (adw_expander_row_get_expanded(row))
    g_hash_table_add(b->p->expanded, g_strdup(b->app_id));
  else
    g_hash_table_remove(b->p->expanded, b->app_id);
}

static GtkWidget *
app_row_new(Page *p, const NssBudget *bd, gboolean editable)
{
  g_autofree gchar *label = nss_wallet_app_label(bd->app_id);
  g_autofree gchar *spent = nss_format_sats(bd->spent_today_msat);
  g_autofree gchar *lim = nss_format_sats(bd->limit_msat_per_day);
  const gchar *access = bd->allow_read && bd->allow_receive ? "Can see balance, create invoices"
                       : bd->allow_read                      ? "Can see balance"
                       : bd->allow_receive                   ? "Can create invoices"
                                                             : "Cannot see balance";
  g_autofree gchar *sub = g_strdup_printf("%s · budget %s/day, %s spent today", access, lim, spent);
  AdwExpanderRow *row = ADW_EXPANDER_ROW(adw_expander_row_new());
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), label);
  adw_expander_row_set_subtitle(row, sub);
  gtk_widget_set_tooltip_text(GTK_WIDGET(row), bd->app_id);

  AppRow *b = g_new0(AppRow, 1);
  b->p = p;
  b->app_id = g_strdup(bd->app_id);
  b->current_msat = bd->limit_msat_per_day / 1000 * 1000;
  g_object_set_data_full(G_OBJECT(row), "nss-app", b, app_row_free);

  AdwSwitchRow *read = ADW_SWITCH_ROW(adw_switch_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(read), "Can see balance and history");
  gboolean shell = g_strcmp0(bd->app_id, NSS_WALLET_SHELL_APP_ID) == 0;
  adw_action_row_set_subtitle(ADW_ACTION_ROW(read),
    shell ? "Applies to every GNOME Shell extension: the wallet agent identifies GNOME Shell, "
            "not individual extensions"
          : "Paying always needs a budget or your approval");
  adw_switch_row_set_active(read, bd->allow_read);
  g_signal_connect(read, "notify::active", G_CALLBACK(on_read_toggled), b);
  gtk_widget_set_sensitive(GTK_WIDGET(read), editable);
  adw_expander_row_add_row(row, GTK_WIDGET(read));

  /* A separate grant (nostrc-muhk): seeing the balance does not let an app
   * create payment requests that look like yours. */
  AdwSwitchRow *recv = ADW_SWITCH_ROW(adw_switch_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(recv), "Can create invoices");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(recv),
    shell ? "Every GNOME Shell extension could request payments into your wallet"
          : "Payment requests into your wallet, without asking you each time");
  adw_switch_row_set_active(recv, bd->allow_receive);
  g_signal_connect(recv, "notify::active", G_CALLBACK(on_receive_toggled), b);
  gtk_widget_set_sensitive(GTK_WIDGET(recv), editable);
  adw_expander_row_add_row(row, GTK_WIDGET(recv));

  b->spin = ADW_SPIN_ROW(adw_spin_row_new_with_range(0, G_MAXUINT32 / 1000, 100));
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(b->spin), "Pay without asking, up to (sats per day)");
  adw_spin_row_set_value(b->spin, (double)(bd->limit_msat_per_day / 1000));
  b->apply = gtk_button_new_with_label("Apply");
  gtk_widget_set_valign(b->apply, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(b->apply, FALSE);
  g_signal_connect(b->apply, "clicked", G_CALLBACK(on_budget_apply), b);
  adw_action_row_add_suffix(ADW_ACTION_ROW(b->spin), b->apply);
  g_signal_connect(b->spin, "notify::value", G_CALLBACK(on_budget_value), b);
  gtk_widget_set_sensitive(GTK_WIDGET(b->spin), editable);
  adw_expander_row_add_row(row, GTK_WIDGET(b->spin));
  adw_expander_row_set_expanded(row, g_hash_table_contains(p->expanded, bd->app_id));
  g_signal_connect(row, "notify::expanded", G_CALLBACK(on_expanded), b);
  return GTK_WIDGET(row);
}

static void
render(Page *p, State *s)
{
  nss_group_clear_dynamic(p->budgets);
  p->trusted = s->trusted;
  gtk_widget_set_visible(p->unpair, s->available && s->paired);
  gtk_widget_set_visible(GTK_WIDGET(p->pair_entry), s->available && !s->paired);
  gtk_widget_set_visible(p->auth_row, s->available && !s->paired && p->auth_uri == NULL);
  if (!s->available) {
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->status), "Wallet agent not available");
    nss_row_set_subtitle_plain(p->status, s->error);
  } else if (s->paired) {
    const gchar *who = s->lud16 && *s->lud16 ? s->lud16 : s->wallet_pubkey;
    g_autofree gchar *t = g_strdup_printf("Connected to %s", who && *who ? who : "a wallet");
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->status), t);
    g_autofree gchar *rel = s->relays ? g_strjoinv(", ", s->relays) : g_strdup("");
    g_autofree gchar *sub = g_strdup_printf("Nostr Wallet Connect via %s", *rel ? rel : "—");
    nss_row_set_subtitle_plain(p->status, sub);
  } else {
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->status), "No wallet connected");
    nss_row_set_subtitle_plain(p->status,
      "Ask your wallet app for a connection, or paste a connection link from it");
  }

  if (s->budgets_error) {
    nss_group_add_dynamic(p->budgets, nss_info_row("Apps unreadable", s->budgets_error));
    return;
  }
  if (!s->trusted && s->available)
    nss_group_add_dynamic(p->budgets, nss_info_row("The wallet agent will confirm every change",
      s->untrusted_why ? s->untrusted_why : "This is not the installed Nostr Settings"));
  /* GNOME Shell gets a standing row: its panel indicator asks for access
   * here instead of prompting. */
  gboolean shell_listed = FALSE;
  for (guint i = 0; i < s->budgets->len; i++)
    shell_listed |= g_strcmp0(((NssBudget *)g_ptr_array_index(s->budgets, i))->app_id,
                              NSS_WALLET_SHELL_APP_ID) == 0;
  if (!shell_listed) {
    NssBudget shell = { .app_id = (gchar *)NSS_WALLET_SHELL_APP_ID };
    nss_group_add_dynamic(p->budgets, app_row_new(p, &shell, s->available));
  }
  for (guint i = 0; i < s->budgets->len; i++)
    nss_group_add_dynamic(p->budgets,
                          app_row_new(p, g_ptr_array_index(s->budgets, i), s->available));
}

static void
loaded(GtkWidget *owner, gpointer result, const GError *error, gpointer data)
{
  (void)error; (void)data;
  Page *p = g_object_get_data(G_OBJECT(owner), "nss-page");
  render(p, result);
}

static void
refresh(Page *p)
{
  if (p->ctx->bus)
    nss_run(p->page, load, loaded, g_object_ref(p->ctx->bus), g_object_unref, state_free);
}

/* Signal callbacks may still run after unsubscribe (until the destroy
 * notify), so the subscription holds a weak ref to the page, not Page*. */
static void
on_agent_changed(GDBusConnection *c, const gchar *sender, const gchar *path,
                 const gchar *iface, const gchar *signal, GVariant *params, gpointer data)
{
  (void)c; (void)sender; (void)path; (void)iface; (void)signal; (void)params;
  g_autoptr(GtkWidget) page = g_weak_ref_get(data);
  Page *p = page ? g_object_get_data(G_OBJECT(page), "nss-page") : NULL;
  if (p)
    refresh(p);
}

static void
weak_ref_free(gpointer data)
{
  g_weak_ref_clear(data);
  g_free(data);
}

/* ── pairing ── */

static void
pair_done(GObject *src, GAsyncResult *res, gpointer data)
{
  GtkWidget *page = data;
  Page *p = g_object_get_data(G_OBJECT(page), "nss-page");
  g_autoptr(GError) err = NULL;
  g_autoptr(GVariant) r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
  if (p) {
    p->busy = FALSE;
    if (r == NULL) {
      g_dbus_error_strip_remote_error(err);
      nss_toast(p->ctx, "%s", err->message);
    }
    refresh(p);
  }
  g_object_unref(page);
}

static void
call_agent(Page *p, const gchar *method, GVariant *params)
{
  if (p->busy || p->ctx->bus == NULL)
    return;
  p->busy = TRUE;
  g_dbus_connection_call(p->ctx->bus, NSS_WALLET_BUS_NAME, NSS_WALLET_OBJ_PATH,
                         NSS_WALLET_IFACE, method, params, NULL, G_DBUS_CALL_FLAGS_NONE,
                         NSS_WALLET_PROMPT_TIMEOUT_MS, NULL, pair_done,
                         g_object_ref(p->page));
}

static void
on_pair(AdwEntryRow *row, gpointer data)
{
  Page *p = data;
  const gchar *uri = gtk_editable_get_text(GTK_EDITABLE(row));
  if (!g_str_has_prefix(uri, "nostr+walletconnect:")) {
    nss_toast(p->ctx, "That is not a nostr+walletconnect: link");
    return;
  }
  call_agent(p, "Pair", g_variant_new("(s)", uri));
  /* The secret is in the link: do not leave it on screen. */
  gtk_editable_set_text(GTK_EDITABLE(row), "");
}

static void
on_unpair(GtkButton *b, gpointer data)
{
  (void)b;
  call_agent(data, "Unpair", NULL);
}

/* ── connect by request (nostr+walletauth: the agent creates the key) ── */

static void
auth_waiting(Page *p, const gchar *uri)
{
  g_free(p->auth_uri);
  p->auth_uri = g_strdup(uri);
  gtk_widget_set_visible(p->wait_row, uri != NULL);
  gtk_widget_set_visible(p->auth_row, uri == NULL);
  if (uri)
    nss_row_set_subtitle_plain(p->wait_row,
      "Approve the connection in your wallet app. If it did not open, copy the link into it.");
}

static void
begin_done(GObject *src, GAsyncResult *res, gpointer data)
{
  GtkWidget *page = data;
  Page *p = g_object_get_data(G_OBJECT(page), "nss-page");
  g_autoptr(GError) err = NULL;
  g_autoptr(GVariant) r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
  if (p) {
    gtk_widget_set_sensitive(p->auth_row, TRUE);
    if (r == NULL) {
      g_dbus_error_strip_remote_error(err);
      nss_toast(p->ctx, "Cannot connect a wallet: %s", err->message);
    } else {
      const gchar *uri = NULL;
      g_variant_get(r, "(&s)", &uri);
      auth_waiting(p, uri);
      g_autoptr(GError) lerr = NULL;
      if (!g_app_info_launch_default_for_uri(uri, NULL, &lerr))
        nss_toast(p->ctx, "No wallet app handles connection requests here; copy the link into your wallet");
    }
  }
  g_object_unref(page);
}

static void
on_auth_clicked(GtkButton *b, gpointer data)
{
  (void)b;
  Page *p = data;
  if (p->ctx->bus == NULL)
    return;
  gtk_widget_set_sensitive(p->auth_row, FALSE);
  g_dbus_connection_call(p->ctx->bus, NSS_WALLET_BUS_NAME, NSS_WALLET_OBJ_PATH, NSS_WALLET_IFACE,
                         "BeginWalletAuth",
                         g_variant_new_parsed("(@a{sv} {'name': <'GNOME (Nostr Wallet)'>},)"),
                         G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 20000, NULL, begin_done,
                         g_object_ref(p->page));
}

static void
on_auth_copy(GtkButton *b, gpointer data)
{
  (void)b;
  Page *p = data;
  if (!p->auth_uri)
    return;
  gdk_clipboard_set_text(gtk_widget_get_clipboard(p->page), p->auth_uri);
  nss_toast(p->ctx, "Link copied — it holds no secret");
}

static void
on_auth_cancel(GtkButton *b, gpointer data)
{
  (void)b;
  Page *p = data;
  if (p->ctx->bus)
    g_dbus_connection_call(p->ctx->bus, NSS_WALLET_BUS_NAME, NSS_WALLET_OBJ_PATH, NSS_WALLET_IFACE,
                           "CancelWalletAuth", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL, NULL,
                           NULL);
  auth_waiting(p, NULL);
}

static void
on_auth_finished(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface,
                 const gchar *signal, GVariant *params, gpointer data)
{
  (void)c; (void)sender; (void)path; (void)iface; (void)signal;
  g_autoptr(GtkWidget) page = g_weak_ref_get(data);
  Page *p = page ? g_object_get_data(G_OBJECT(page), "nss-page") : NULL;
  if (!p || !p->auth_uri)
    return; /* not our request */
  gboolean ok = FALSE;
  const gchar *msg = NULL;
  g_variant_get(params, "(b&s)", &ok, &msg);
  auth_waiting(p, NULL);
  if (ok)
    nss_toast(p->ctx, "Wallet connected");
  else
    nss_toast(p->ctx, "Wallet not connected: %s", msg);
  refresh(p);
}

AdwPreferencesPage *
nss_page_wallet_new(NssContext *ctx)
{
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
  adw_preferences_page_set_title(page, "Wallet");
  adw_preferences_page_set_icon_name(page, "thunderbolt-symbolic");
  Page *p = g_new0(Page, 1);
  p->ctx = nss_context_ref(ctx);
  p->page = GTK_WIDGET(page);
  p->expanded = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_object_set_data_full(G_OBJECT(page), "nss-page", p, page_free);

  AdwPreferencesGroup *g = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(g, "Lightning wallet");
  adw_preferences_group_set_description(g,
    "One Nostr Wallet Connect (NIP-47) wallet shared by every app. Connecting and "
    "disconnecting are always confirmed by the wallet agent.");
  p->status = nss_info_row("Checking…", NULL);
  p->unpair = gtk_button_new_with_label("Disconnect…");
  gtk_widget_add_css_class(p->unpair, "destructive-action");
  gtk_widget_set_valign(p->unpair, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(p->unpair, FALSE);
  g_signal_connect(p->unpair, "clicked", G_CALLBACK(on_unpair), p);
  adw_action_row_add_suffix(ADW_ACTION_ROW(p->status), p->unpair);
  adw_preferences_group_add(g, p->status);

  p->auth_row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->auth_row), "Connect with your wallet app");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(p->auth_row),
    "Recommended: the connection key is created on this computer, so nobody else holds it");
  GtkWidget *auth_btn = gtk_button_new_with_label("Connect…");
  gtk_widget_add_css_class(auth_btn, "suggested-action");
  gtk_widget_set_valign(auth_btn, GTK_ALIGN_CENTER);
  g_signal_connect(auth_btn, "clicked", G_CALLBACK(on_auth_clicked), p);
  adw_action_row_add_suffix(ADW_ACTION_ROW(p->auth_row), auth_btn);
  gtk_widget_set_visible(p->auth_row, FALSE);
  adw_preferences_group_add(g, p->auth_row);

  p->wait_row = nss_info_row("Waiting for your wallet…", NULL);
  GtkWidget *copy_btn = nss_suffix_button("edit-copy-symbolic", "Copy the connection request link");
  g_signal_connect(copy_btn, "clicked", G_CALLBACK(on_auth_copy), p);
  adw_action_row_add_suffix(ADW_ACTION_ROW(p->wait_row), copy_btn);
  GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
  gtk_widget_set_valign(cancel_btn, GTK_ALIGN_CENTER);
  g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_auth_cancel), p);
  adw_action_row_add_suffix(ADW_ACTION_ROW(p->wait_row), cancel_btn);
  gtk_widget_set_visible(p->wait_row, FALSE);
  adw_preferences_group_add(g, p->wait_row);

  p->pair_entry = ADW_ENTRY_ROW(adw_password_entry_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->pair_entry),
                                "Or paste a connection link (nostr+walletconnect://…)");
  adw_entry_row_set_show_apply_button(p->pair_entry, TRUE);
  gtk_widget_set_visible(GTK_WIDGET(p->pair_entry), FALSE);
  g_signal_connect(p->pair_entry, "apply", G_CALLBACK(on_pair), p);
  g_signal_connect(p->pair_entry, "entry-activated", G_CALLBACK(on_pair), p);
  adw_preferences_group_add(g, GTK_WIDGET(p->pair_entry));
  adw_preferences_page_add(page, g);

  p->budgets = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(p->budgets, "Apps and websites");
  adw_preferences_group_set_description(p->budgets,
    "What each app (or website, through the browser extension) may do without asking: see "
    "your balance and history, and pay up to a daily amount (0 always asks).");
  adw_preferences_page_add(page, p->budgets);

  if (ctx->bus) {
    GWeakRef *wr = g_new0(GWeakRef, 1);
    g_weak_ref_init(wr, page);
    p->props_sub = g_dbus_connection_signal_subscribe(
      ctx->bus, NSS_WALLET_BUS_NAME, "org.freedesktop.DBus.Properties", "PropertiesChanged",
      NSS_WALLET_OBJ_PATH, NSS_WALLET_IFACE, G_DBUS_SIGNAL_FLAGS_NONE, on_agent_changed, wr,
      weak_ref_free);
    GWeakRef *wr2 = g_new0(GWeakRef, 1);
    g_weak_ref_init(wr2, page);
    p->apps_sub = g_dbus_connection_signal_subscribe(
      ctx->bus, NSS_WALLET_BUS_NAME, NSS_WALLET_IFACE, "AppsChanged", NSS_WALLET_OBJ_PATH, NULL,
      G_DBUS_SIGNAL_FLAGS_NONE, on_agent_changed, wr2, weak_ref_free);
    GWeakRef *wr3 = g_new0(GWeakRef, 1);
    g_weak_ref_init(wr3, page);
    p->auth_sub = g_dbus_connection_signal_subscribe(
      ctx->bus, NSS_WALLET_BUS_NAME, NSS_WALLET_IFACE, "WalletAuthFinished", NSS_WALLET_OBJ_PATH, NULL,
      G_DBUS_SIGNAL_FLAGS_NONE, on_auth_finished, wr3, weak_ref_free);
    refresh(p);
  }
  return page;
}
