/* nss-page-wallet.c — org.nostr.Wallet1 pairing + per-app budgets.
 * SPDX-License-Identifier: MIT
 *
 * Pair / Unpair / SetBudget are forwarded to the agent, which shows its
 * own confirmation dialog (always, for pairing); this page never sees the
 * pairing secret. Budgets are listed from the agent's budgets.json
 * (read-only; see nss-wallet.h for why not GetBudget).
 */
#include "nss-ui.h"
#include "nss-wallet.h"

typedef struct {
  NssContext          *ctx;
  GtkWidget           *page;
  GtkWidget           *status;
  GtkWidget           *unpair;
  AdwEntryRow         *pair_entry;
  AdwPreferencesGroup *budgets;
  guint                props_sub;
  gboolean             busy;
} Page;

static void refresh(Page *p);

static void
page_free(gpointer data)
{
  Page *p = data;
  if (p->props_sub && p->ctx->bus)
    g_dbus_connection_signal_unsubscribe(p->ctx->bus, p->props_sub);
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
  }
  g_autofree gchar *path = nss_wallet_budgets_path();
  s->budgets = nss_wallet_budgets_load(path, NULL, &e);
  if (s->budgets == NULL) {
    s->budgets_error = g_strdup(e ? e->message : "unreadable");
    g_clear_error(&e);
  }
  return s;
}

/* ── budgets ── */

typedef struct {
  Page    *p;
  gchar   *app_id;
  guint64  current_msat;
  GtkWidget *apply;
  AdwSpinRow *spin;
} BudgetRow;

static void
budget_row_free(gpointer d)
{
  BudgetRow *b = d;
  g_free(b->app_id);
  g_free(b);
}

static void
on_budget_value(AdwSpinRow *row, GParamSpec *ps, gpointer data)
{
  (void)ps;
  BudgetRow *b = data;
  guint64 msat = (guint64)adw_spin_row_get_value(row) * 1000;
  gtk_widget_set_visible(b->apply, msat != b->current_msat);
}

static void
set_budget_done(GObject *src, GAsyncResult *res, gpointer data)
{
  GtkWidget *page = data;
  Page *p = g_object_get_data(G_OBJECT(page), "nss-page");
  g_autoptr(GError) err = NULL;
  g_autoptr(GVariant) r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
  if (p) {
    if (r == NULL) {
      g_dbus_error_strip_remote_error(err);
      nss_toast(p->ctx, "Budget not changed: %s", err->message);
    } else {
      nss_toast(p->ctx, "Budget updated");
    }
    refresh(p);
  }
  g_object_unref(page);
}

static void
on_budget_apply(GtkButton *btn, gpointer data)
{
  (void)btn;
  BudgetRow *b = data;
  guint64 sats = (guint64)adw_spin_row_get_value(b->spin);
  guint32 msat = (guint32)MIN(sats * 1000, (guint64)G_MAXUINT32);
  gtk_widget_set_sensitive(b->apply, FALSE);
  nss_toast(b->p->ctx, "Confirm the change in the wallet dialog");
  g_dbus_connection_call(b->p->ctx->bus, NSS_WALLET_BUS_NAME, NSS_WALLET_OBJ_PATH,
                         NSS_WALLET_IFACE, "SetBudget", g_variant_new("(su)", b->app_id, msat),
                         NULL, G_DBUS_CALL_FLAGS_NONE, NSS_WALLET_PROMPT_TIMEOUT_MS, NULL,
                         set_budget_done, g_object_ref(b->p->page));
}

static void
render(Page *p, State *s)
{
  nss_group_clear_dynamic(p->budgets);
  gtk_widget_set_visible(p->unpair, s->available && s->paired);
  gtk_widget_set_visible(GTK_WIDGET(p->pair_entry), s->available && !s->paired);
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
      "Paste a connection link from your wallet, or open a nostr+walletconnect: link");
  }

  if (s->budgets_error) {
    nss_group_add_dynamic(p->budgets, nss_info_row("Budgets unreadable", s->budgets_error));
    return;
  }
  if (s->budgets->len == 0) {
    nss_group_add_dynamic(p->budgets, nss_info_row("No app has a budget yet",
      "Apps get one when you tick “Always allow up to …” in a payment dialog"));
    return;
  }
  for (guint i = 0; i < s->budgets->len; i++) {
    NssBudget *bd = g_ptr_array_index(s->budgets, i);
    g_autofree gchar *label = nss_wallet_app_label(bd->app_id);
    g_autofree gchar *spent = nss_format_sats(bd->spent_today_msat);
    g_autofree gchar *sub = g_strdup_printf("Spent today: %s · %s", spent,
                                            bd->allow_read ? "can see balance and history"
                                                           : "cannot see balance");
    AdwSpinRow *row = ADW_SPIN_ROW(adw_spin_row_new_with_range(0, G_MAXUINT32 / 1000, 100));
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), label);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
    gtk_widget_set_tooltip_text(GTK_WIDGET(row), bd->app_id);
    adw_spin_row_set_value(row, (double)(bd->limit_msat_per_day / 1000));
    BudgetRow *b = g_new0(BudgetRow, 1);
    b->p = p;
    b->app_id = g_strdup(bd->app_id);
    b->current_msat = bd->limit_msat_per_day / 1000 * 1000;
    b->spin = row;
    b->apply = gtk_button_new_with_label("Apply");
    gtk_widget_set_valign(b->apply, GTK_ALIGN_CENTER);
    gtk_widget_set_visible(b->apply, FALSE);
    g_signal_connect(b->apply, "clicked", G_CALLBACK(on_budget_apply), b);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), b->apply);
    g_object_set_data_full(G_OBJECT(row), "nss-budget", b, budget_row_free);
    g_signal_connect(row, "notify::value", G_CALLBACK(on_budget_value), b);
    gtk_widget_set_sensitive(GTK_WIDGET(row), s->available);
    nss_group_add_dynamic(p->budgets, GTK_WIDGET(row));
  }
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
on_props_changed(GDBusConnection *c, const gchar *sender, const gchar *path,
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

AdwPreferencesPage *
nss_page_wallet_new(NssContext *ctx)
{
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
  adw_preferences_page_set_title(page, "Wallet");
  adw_preferences_page_set_icon_name(page, "thunderbolt-symbolic");
  Page *p = g_new0(Page, 1);
  p->ctx = nss_context_ref(ctx);
  p->page = GTK_WIDGET(page);
  g_object_set_data_full(G_OBJECT(page), "nss-page", p, page_free);

  AdwPreferencesGroup *g = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(g, "Lightning wallet");
  adw_preferences_group_set_description(g,
    "One Nostr Wallet Connect (NIP-47) wallet shared by every app. Connecting and "
    "disconnecting are confirmed by the wallet agent.");
  p->status = nss_info_row("Checking…", NULL);
  p->unpair = gtk_button_new_with_label("Disconnect…");
  gtk_widget_add_css_class(p->unpair, "destructive-action");
  gtk_widget_set_valign(p->unpair, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(p->unpair, FALSE);
  g_signal_connect(p->unpair, "clicked", G_CALLBACK(on_unpair), p);
  adw_action_row_add_suffix(ADW_ACTION_ROW(p->status), p->unpair);
  adw_preferences_group_add(g, p->status);
  p->pair_entry = ADW_ENTRY_ROW(adw_password_entry_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->pair_entry),
                                "Connect a wallet (nostr+walletconnect://…)");
  adw_entry_row_set_show_apply_button(p->pair_entry, TRUE);
  gtk_widget_set_visible(GTK_WIDGET(p->pair_entry), FALSE);
  g_signal_connect(p->pair_entry, "apply", G_CALLBACK(on_pair), p);
  g_signal_connect(p->pair_entry, "entry-activated", G_CALLBACK(on_pair), p);
  adw_preferences_group_add(g, GTK_WIDGET(p->pair_entry));
  adw_preferences_page_add(page, g);

  p->budgets = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(p->budgets, "Daily budgets (sats per day)");
  adw_preferences_group_set_description(p->budgets,
    "Apps may pay without asking up to this amount per day; 0 always asks. "
    "Changes are confirmed in the wallet agent's dialog.");
  adw_preferences_page_add(page, p->budgets);

  if (ctx->bus) {
    GWeakRef *wr = g_new0(GWeakRef, 1);
    g_weak_ref_init(wr, page);
    p->props_sub = g_dbus_connection_signal_subscribe(
      ctx->bus, NSS_WALLET_BUS_NAME, "org.freedesktop.DBus.Properties", "PropertiesChanged",
      NSS_WALLET_OBJ_PATH, NSS_WALLET_IFACE, G_DBUS_SIGNAL_FLAGS_NONE, on_props_changed, wr,
      weak_ref_free);
    refresh(p);
  }
  return page;
}
