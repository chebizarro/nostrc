/* nwa-ui.c - see nwa-ui.h
 *
 * SPDX-License-Identifier: MIT
 */
#include "nwa-ui.h"

#include <adwaita.h>
#include <string.h>

static gint ui_state; /* 0 = untried, 1 = ready, -1 = unavailable */

gboolean
nwa_ui_available(void)
{
  if (ui_state == 0) {
    const gchar *headless = g_getenv("NOSTR_WALLET_AGENT_HEADLESS");
    if (headless && *headless && g_strcmp0(headless, "0") != 0) {
      ui_state = -1;
    } else if (gtk_init_check()) {
      adw_init();
      ui_state = 1;
    } else {
      g_message("nostr-wallet-agent: no display; requests that need approval will be denied");
      ui_state = -1;
    }
  }
  return ui_state > 0;
}

gchar *
nwa_ui_format_msat(guint64 msat)
{
  guint64 sats = msat / 1000;
  guint64 rem = msat % 1000;
  GString *s = g_string_new(NULL);
  /* thousands separators without locale surprises */
  g_autofree gchar *digits = g_strdup_printf("%" G_GUINT64_FORMAT, sats);
  gsize n = strlen(digits);
  for (gsize i = 0; i < n; i++) {
    if (i > 0 && (n - i) % 3 == 0) g_string_append_c(s, ',');
    g_string_append_c(s, digits[i]);
  }
  if (rem) {
    g_autofree gchar *frac = g_strdup_printf("%03" G_GUINT64_FORMAT, rem);
    gsize fl = strlen(frac);
    while (fl > 0 && frac[fl - 1] == '0') frac[--fl] = '\0';
    g_string_append_printf(s, ".%s", frac);
  }
  g_string_append(s, sats == 1 && rem == 0 ? " sat" : " sats");
  return g_string_free(s, FALSE);
}

/* ---- shared once-only answer plumbing ---- */

typedef struct {
  GtkWindow *window;
  guint      timeout_id;
  gboolean   answered;
  /* payment */
  NwaPaymentPromptCallback pay_cb;
  AdwSwitchRow *remember_row;
  AdwSpinRow   *limit_row;
  /* confirm */
  NwaConfirmCallback confirm_cb;
  gpointer user_data;
} Dialog;

static void
dialog_answer_full(Dialog *d, gboolean yes, gboolean destroy)
{
  if (d->answered) return;
  d->answered = TRUE;
  if (d->timeout_id) {
    g_source_remove(d->timeout_id);
    d->timeout_id = 0;
  }
  if (d->pay_cb) {
    gboolean remember = yes && d->remember_row &&
                        adw_switch_row_get_active(d->remember_row);
    guint64 limit = remember ? (guint64)adw_spin_row_get_value(d->limit_row) * 1000 : 0;
    d->pay_cb(yes, remember, limit, d->user_data);
  } else if (d->confirm_cb) {
    gboolean remember = yes && d->remember_row && adw_switch_row_get_active(d->remember_row);
    d->confirm_cb(yes, remember, d->user_data);
  }
  if (destroy)
    gtk_window_destroy(d->window);
}

static void
dialog_answer(Dialog *d, gboolean yes)
{
  dialog_answer_full(d, yes, TRUE);
}

static gboolean
on_close_request(GtkWindow *w, gpointer data)
{
  (void)w;
  /* the default handler destroys the window after we return */
  dialog_answer_full(data, FALSE, FALSE);
  return FALSE;
}

static gboolean
on_dialog_timeout(gpointer data)
{
  Dialog *d = data;
  d->timeout_id = 0;
  dialog_answer(d, FALSE);
  return G_SOURCE_REMOVE;
}

static void on_yes(GtkButton *b, gpointer data) { (void)b; dialog_answer(data, TRUE); }
static void on_no(GtkButton *b, gpointer data)  { (void)b; dialog_answer(data, FALSE); }

static void
on_remember_toggled(GObject *row, GParamSpec *pspec, gpointer data)
{
  (void)pspec;
  Dialog *d = data;
  gtk_widget_set_sensitive(GTK_WIDGET(d->limit_row),
                           adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
}

static Dialog *
dialog_new(const gchar *title, GtkWidget *content, const gchar *yes_label,
           gboolean destructive, guint timeout_s)
{
  Dialog *d = g_new0(Dialog, 1);
  AdwWindow *win = ADW_WINDOW(adw_window_new());
  d->window = GTK_WINDOW(win);
  gtk_window_set_title(d->window, title);
  gtk_window_set_default_size(d->window, 440, -1);
  gtk_window_set_resizable(d->window, FALSE);

  GtkWidget *view = adw_toolbar_view_new();
  GtkWidget *header = adw_header_bar_new();
  adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
  adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 18);
  gtk_widget_set_margin_start(box, 18);
  gtk_widget_set_margin_end(box, 18);
  gtk_box_append(GTK_BOX(box), content);

  GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_set_homogeneous(GTK_BOX(buttons), TRUE);
  GtkWidget *no = gtk_button_new_with_mnemonic("_Deny");
  GtkWidget *yes = gtk_button_new_with_mnemonic(yes_label);
  gtk_widget_add_css_class(no, "pill");
  gtk_widget_add_css_class(yes, "pill");
  gtk_widget_add_css_class(yes, destructive ? "destructive-action" : "suggested-action");
  gtk_box_append(GTK_BOX(buttons), no);
  gtk_box_append(GTK_BOX(buttons), yes);
  gtk_box_append(GTK_BOX(box), buttons);

  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), box);
  adw_window_set_content(win, view);

  g_signal_connect(no, "clicked", G_CALLBACK(on_no), d);
  g_signal_connect(yes, "clicked", G_CALLBACK(on_yes), d);
  g_signal_connect(d->window, "close-request", G_CALLBACK(on_close_request), d);
  g_object_set_data_full(G_OBJECT(d->window), "nwa-dialog", d, g_free);
  /* Deny is the default: Enter must never pay by accident. */
  gtk_window_set_default_widget(d->window, no);
  if (timeout_s)
    d->timeout_id = g_timeout_add_seconds(timeout_s, on_dialog_timeout, d);
  return d;
}

static GtkWidget *
info_row(const gchar *title, const gchar *value)
{
  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), value);
  adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), TRUE);
  gtk_widget_add_css_class(row, "property");
  return row;
}

void
nwa_ui_prompt_payment(const NwaPaymentPrompt *p, guint timeout_s,
                      NwaPaymentPromptCallback callback, gpointer user_data)
{
  if (!nwa_ui_available()) {
    callback(FALSE, FALSE, 0, user_data);
    return;
  }
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);

  g_autofree gchar *amount = nwa_ui_format_msat(p->amount_msat);
  GtkWidget *amount_label = gtk_label_new(amount);
  gtk_widget_add_css_class(amount_label, "title-1");
  gtk_box_append(GTK_BOX(content), amount_label);

  g_autofree gchar *lead = p->via_link
    ? g_strdup_printf("A payment link opened by %s", p->app_name)
    : g_strdup_printf("%s wants to pay a Lightning invoice", p->app_name);
  GtkWidget *lead_label = gtk_label_new(lead);
  gtk_label_set_wrap(GTK_LABEL(lead_label), TRUE);
  gtk_label_set_justify(GTK_LABEL(lead_label), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class(lead_label, "dim-label");
  gtk_box_append(GTK_BOX(content), lead_label);

  if (p->over_budget) {
    GtkWidget *warn = gtk_label_new("This payment exceeds the app's daily budget.");
    gtk_label_set_wrap(GTK_LABEL(warn), TRUE);
    gtk_widget_add_css_class(warn, "warning");
    gtk_box_append(GTK_BOX(content), warn);
  }

  GtkWidget *group = adw_preferences_group_new();
  g_autofree gchar *who = p->app_id
    ? g_strdup_printf("%s (%s%s)", p->app_id, p->app_kind,
                      p->app_attested ? ", verified by sandbox" : ", unverified")
    : g_strdup("Could not identify the requesting application");
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), info_row("Requested by", who));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                            info_row("Description", p->description && *p->description
                                                      ? p->description : "(none)"));
  if (p->payee)
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), info_row("Recipient node", p->payee));
  if (p->network && g_strcmp0(p->network, "bc") != 0) {
    g_autofree gchar *net = g_strdup_printf("%s (not Bitcoin mainnet)", p->network);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), info_row("Network", net));
  }
  g_autofree gchar *budget = NULL;
  if (p->limit_msat) {
    g_autofree gchar *rem = nwa_ui_format_msat(p->remaining_msat);
    g_autofree gchar *lim = nwa_ui_format_msat(p->limit_msat);
    budget = g_strdup_printf("%s of %s left today", rem, lim);
  } else {
    budget = g_strdup("No automatic payments allowed");
  }
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), info_row("Budget", budget));
  gtk_box_append(GTK_BOX(content), group);

  Dialog *d = NULL;
  AdwSwitchRow *remember = NULL;
  AdwSpinRow *limit = NULL;
  if (p->can_remember) {
    GtkWidget *rg = adw_preferences_group_new();
    remember = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(remember), "Always allow this app");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(remember),
                                "Pay without asking while within the daily limit");
    guint64 amount_sats = (p->amount_msat + 999) / 1000;
    gdouble def = p->limit_msat ? (gdouble)(p->limit_msat / 1000) : 1000.0;
    if (def < (gdouble)amount_sats) def = (gdouble)amount_sats;
    limit = ADW_SPIN_ROW(adw_spin_row_new_with_range(1, 100000000, 100));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(limit), "Up to (sats per day)");
    adw_spin_row_set_value(limit, def);
    gtk_widget_set_sensitive(GTK_WIDGET(limit), FALSE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(rg), GTK_WIDGET(remember));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(rg), GTK_WIDGET(limit));
    gtk_box_append(GTK_BOX(content), rg);
  }

  d = dialog_new("Payment Request", content, "_Pay", FALSE, timeout_s);
  d->pay_cb = callback;
  d->user_data = user_data;
  d->remember_row = remember;
  d->limit_row = limit;
  if (remember)
    g_signal_connect(remember, "notify::active", G_CALLBACK(on_remember_toggled), d);
  gtk_window_present(d->window);
}

void
nwa_ui_confirm(const gchar *title, const gchar *body, const gchar *accept_label,
               gboolean destructive, const gchar *remember_label, guint timeout_s,
               NwaConfirmCallback callback, gpointer user_data)
{
  if (!nwa_ui_available()) {
    callback(FALSE, FALSE, user_data);
    return;
  }
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  GtkWidget *label = gtk_label_new(body);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 48);
  gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
  gtk_label_set_selectable(GTK_LABEL(label), TRUE);
  gtk_box_append(GTK_BOX(content), label);
  AdwSwitchRow *remember = NULL;
  if (remember_label) {
    GtkWidget *group = adw_preferences_group_new();
    remember = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(remember), remember_label);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), GTK_WIDGET(remember));
    gtk_box_append(GTK_BOX(content), group);
  }
  Dialog *d = dialog_new(title, content, accept_label, destructive, timeout_s);
  d->confirm_cb = callback;
  d->user_data = user_data;
  d->remember_row = remember;
  gtk_window_present(d->window);
}

static gboolean
add_toast_idle(gpointer data)
{
  GtkWidget *overlay = data;
  const gchar *msg = g_object_get_data(G_OBJECT(overlay), "nwa-toast");
  if (msg) adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(overlay), adw_toast_new(msg));
  g_object_unref(overlay);
  return G_SOURCE_REMOVE;
}

void
nwa_ui_show_message(const gchar *title, const gchar *body, const gchar *toast)
{
  if (!nwa_ui_available()) {
    g_message("nostr-wallet-agent: %s: %s", title, toast ? toast : body);
    return;
  }
  AdwWindow *win = ADW_WINDOW(adw_window_new());
  gtk_window_set_title(GTK_WINDOW(win), title);
  gtk_window_set_default_size(GTK_WINDOW(win), 440, 320);
  GtkWidget *view = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), adw_header_bar_new());
  GtkWidget *status = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(status), "dialog-information-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(status), title);
  /* the description is Pango markup; body may carry untrusted URI text */
  g_autofree gchar *escaped = g_markup_escape_text(body ? body : "", -1);
  adw_status_page_set_description(ADW_STATUS_PAGE(status), escaped);
  GtkWidget *overlay = adw_toast_overlay_new();
  adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(overlay), status);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), overlay);
  adw_window_set_content(win, view);
  if (toast) {
    g_object_set_data_full(G_OBJECT(overlay), "nwa-toast", g_strdup(toast), g_free);
    g_idle_add(add_toast_idle, g_object_ref(overlay));
  }
  gtk_window_present(GTK_WINDOW(win));
}
