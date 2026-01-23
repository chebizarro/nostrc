/* sheet-delegation.c - NIP-26 delegation management dialog implementation */
#include "sheet-delegation.h"
#include "../app-resources.h"
#include "../../delegation.h"
#include "../../accounts_store.h"
#include <nostr/nip19/nip19.h>
#include <string.h>
#include <time.h>

struct _SheetDelegation {
  AdwDialog parent_instance;

  /* Template children - Main */
  GtkWidget *stack_main;
  GtkWidget *btn_back;
  GtkWidget *btn_close;

  /* Template children - List page */
  AdwStatusPage *status_header;
  GtkWidget *btn_create_new;
  AdwPreferencesGroup *group_active;
  GtkListBox *list_delegations;
  AdwStatusPage *status_empty;

  /* Template children - Create page */
  AdwBanner *banner_info;
  AdwEntryRow *entry_delegatee;
  AdwEntryRow *entry_label;
  GtkCheckButton *chk_all_kinds;
  AdwExpanderRow *expander_kinds;
  GtkCheckButton *chk_kind_0;
  GtkCheckButton *chk_kind_1;
  GtkCheckButton *chk_kind_3;
  GtkCheckButton *chk_kind_4;
  GtkCheckButton *chk_kind_6;
  GtkCheckButton *chk_kind_7;
  AdwEntryRow *entry_custom_kind;
  GtkCheckButton *chk_no_time_limit;
  AdwExpanderRow *expander_time;
  GtkDropDown *dropdown_from_preset;
  GtkDropDown *dropdown_until_preset;
  GtkWidget *btn_cancel_create;
  GtkWidget *btn_create;

  /* Template children - Result page */
  AdwStatusPage *status_result;
  GtkLabel *lbl_delegation_tag;
  GtkWidget *btn_copy_tag;
  GtkLabel *lbl_result_delegatee;
  GtkLabel *lbl_result_kinds;
  GtkLabel *lbl_result_validity;
  GtkWidget *btn_done;

  /* Template children - Details page */
  GtkLabel *lbl_detail_label;
  GtkLabel *lbl_detail_delegatee;
  GtkLabel *lbl_detail_kinds;
  GtkLabel *lbl_detail_from;
  GtkLabel *lbl_detail_until;
  GtkLabel *lbl_detail_created;
  GtkLabel *lbl_detail_status;
  GtkLabel *lbl_detail_tag;
  GtkWidget *btn_copy_detail_tag;
  GtkWidget *btn_back_to_list;
  GtkWidget *btn_revoke;

  /* Internal state */
  gchar *npub;
  gchar *current_delegation_id;  /* For details view */
  GnDelegation *created_delegation;  /* Last created delegation */

  /* Callbacks */
  SheetDelegationChangedCb on_changed;
  gpointer on_changed_ud;
};

G_DEFINE_TYPE(SheetDelegation, sheet_delegation, ADW_TYPE_DIALOG)

/* Forward declarations */
static void on_create_new(GtkButton *btn, gpointer user_data);
static void on_back(GtkButton *btn, gpointer user_data);
static void on_close(GtkButton *btn, gpointer user_data);
static void on_cancel_create(GtkButton *btn, gpointer user_data);
static void on_create(GtkButton *btn, gpointer user_data);
static void on_copy_tag(GtkButton *btn, gpointer user_data);
static void on_done(GtkButton *btn, gpointer user_data);
static void on_copy_detail_tag(GtkButton *btn, gpointer user_data);
static void on_back_to_list(GtkButton *btn, gpointer user_data);
static void on_revoke(GtkButton *btn, gpointer user_data);
static void on_delegatee_changed(GtkEditable *editable, gpointer user_data);
static void on_all_kinds_toggled(GtkCheckButton *btn, gpointer user_data);
static void on_no_time_limit_toggled(GtkCheckButton *btn, gpointer user_data);
static void on_delegation_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data);
static void populate_delegation_list(SheetDelegation *self);
static void update_create_button_sensitivity(SheetDelegation *self);

/* Time preset values */
typedef struct {
  const gchar *label;
  gint64 offset_seconds;
} TimePreset;

static const TimePreset from_presets[] = {
  { "Now", 0 },
  { "In 1 hour", 3600 },
  { "In 24 hours", 86400 },
  { "In 1 week", 604800 },
  { NULL, 0 }
};

static const TimePreset until_presets[] = {
  { "Never", 0 },
  { "In 1 hour", 3600 },
  { "In 24 hours", 86400 },
  { "In 1 week", 604800 },
  { "In 30 days", 2592000 },
  { "In 1 year", 31536000 },
  { NULL, 0 }
};

static void sheet_delegation_dispose(GObject *object) {
  SheetDelegation *self = SHEET_DELEGATION(object);

  g_clear_pointer(&self->npub, g_free);
  g_clear_pointer(&self->current_delegation_id, g_free);

  if (self->created_delegation) {
    gn_delegation_free(self->created_delegation);
    self->created_delegation = NULL;
  }

  G_OBJECT_CLASS(sheet_delegation_parent_class)->dispose(object);
}

static void sheet_delegation_class_init(SheetDelegationClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = sheet_delegation_dispose;

  gtk_widget_class_set_template_from_resource(widget_class,
      APP_RESOURCE_PATH "/ui/sheets/sheet-delegation.ui");

  /* Bind template children - Main */
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, stack_main);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, btn_back);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, btn_close);

  /* Bind template children - List page */
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, status_header);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, btn_create_new);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, group_active);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, list_delegations);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, status_empty);

  /* Bind template children - Create page */
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, banner_info);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, entry_delegatee);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, entry_label);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, chk_all_kinds);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, expander_kinds);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, chk_kind_0);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, chk_kind_1);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, chk_kind_3);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, chk_kind_4);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, chk_kind_6);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, chk_kind_7);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, entry_custom_kind);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, chk_no_time_limit);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, expander_time);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, dropdown_from_preset);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, dropdown_until_preset);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, btn_cancel_create);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, btn_create);

  /* Bind template children - Result page */
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, status_result);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, lbl_delegation_tag);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, btn_copy_tag);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, lbl_result_delegatee);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, lbl_result_kinds);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, lbl_result_validity);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, btn_done);

  /* Bind template children - Details page */
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, lbl_detail_label);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, lbl_detail_delegatee);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, lbl_detail_kinds);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, lbl_detail_from);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, lbl_detail_until);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, lbl_detail_created);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, lbl_detail_status);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, lbl_detail_tag);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, btn_copy_detail_tag);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, btn_back_to_list);
  gtk_widget_class_bind_template_child(widget_class, SheetDelegation, btn_revoke);
}

static void setup_time_dropdowns(SheetDelegation *self) {
  /* Setup from presets */
  GtkStringList *from_model = gtk_string_list_new(NULL);
  for (gint i = 0; from_presets[i].label; i++) {
    gtk_string_list_append(from_model, from_presets[i].label);
  }
  gtk_drop_down_set_model(self->dropdown_from_preset, G_LIST_MODEL(from_model));
  g_object_unref(from_model);

  /* Setup until presets */
  GtkStringList *until_model = gtk_string_list_new(NULL);
  for (gint i = 0; until_presets[i].label; i++) {
    gtk_string_list_append(until_model, until_presets[i].label);
  }
  gtk_drop_down_set_model(self->dropdown_until_preset, G_LIST_MODEL(until_model));
  g_object_unref(until_model);
}

static void sheet_delegation_init(SheetDelegation *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  /* Connect signals */
  g_signal_connect(self->btn_create_new, "clicked", G_CALLBACK(on_create_new), self);
  g_signal_connect(self->btn_back, "clicked", G_CALLBACK(on_back), self);
  g_signal_connect(self->btn_close, "clicked", G_CALLBACK(on_close), self);
  g_signal_connect(self->btn_cancel_create, "clicked", G_CALLBACK(on_cancel_create), self);
  g_signal_connect(self->btn_create, "clicked", G_CALLBACK(on_create), self);
  g_signal_connect(self->btn_copy_tag, "clicked", G_CALLBACK(on_copy_tag), self);
  g_signal_connect(self->btn_done, "clicked", G_CALLBACK(on_done), self);
  g_signal_connect(self->btn_copy_detail_tag, "clicked", G_CALLBACK(on_copy_detail_tag), self);
  g_signal_connect(self->btn_back_to_list, "clicked", G_CALLBACK(on_back_to_list), self);
  g_signal_connect(self->btn_revoke, "clicked", G_CALLBACK(on_revoke), self);

  /* Input validation */
  g_signal_connect(self->entry_delegatee, "changed", G_CALLBACK(on_delegatee_changed), self);

  /* Kind selection toggles */
  g_signal_connect(self->chk_all_kinds, "toggled", G_CALLBACK(on_all_kinds_toggled), self);

  /* Time constraint toggles */
  g_signal_connect(self->chk_no_time_limit, "toggled", G_CALLBACK(on_no_time_limit_toggled), self);

  /* Delegation list selection */
  g_signal_connect(self->list_delegations, "row-activated", G_CALLBACK(on_delegation_row_activated), self);

  /* Setup time dropdowns */
  setup_time_dropdowns(self);

  /* Initial state */
  gtk_widget_set_visible(GTK_WIDGET(self->btn_back), FALSE);
}

SheetDelegation *sheet_delegation_new(void) {
  return g_object_new(TYPE_SHEET_DELEGATION, NULL);
}

void sheet_delegation_set_account(SheetDelegation *self, const gchar *npub) {
  g_return_if_fail(SHEET_IS_DELEGATION(self));

  g_free(self->npub);
  self->npub = g_strdup(npub);

  /* Update title with account info */
  if (npub && *npub) {
    AccountsStore *as = accounts_store_get_default();
    gchar *name = accounts_store_get_display_name(as, npub);
    gchar *desc = g_strdup_printf("Manage delegations for %s", name ? name : npub);
    adw_status_page_set_description(self->status_header, desc);
    g_free(desc);
    g_free(name);
  }

  /* Populate delegation list */
  populate_delegation_list(self);
}

void sheet_delegation_set_on_changed(SheetDelegation *self,
                                      SheetDelegationChangedCb callback,
                                      gpointer user_data) {
  g_return_if_fail(SHEET_IS_DELEGATION(self));

  self->on_changed = callback;
  self->on_changed_ud = user_data;
}

void sheet_delegation_refresh(SheetDelegation *self) {
  g_return_if_fail(SHEET_IS_DELEGATION(self));
  populate_delegation_list(self);
}

void sheet_delegation_show_create(SheetDelegation *self) {
  g_return_if_fail(SHEET_IS_DELEGATION(self));

  gtk_stack_set_visible_child_name(GTK_STACK(self->stack_main), "create");
  gtk_widget_set_visible(GTK_WIDGET(self->btn_back), TRUE);
}

/* ======== Internal Functions ======== */

static gchar *format_timestamp(gint64 ts) {
  if (ts <= 0) return g_strdup("N/A");

  time_t t = (time_t)ts;
  struct tm *tm = localtime(&t);
  gchar buf[64];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", tm);
  return g_strdup(buf);
}

static gchar *format_kinds(GArray *kinds) {
  if (!kinds || kinds->len == 0) {
    return g_strdup("All kinds");
  }

  GString *s = g_string_new(NULL);
  for (guint i = 0; i < kinds->len; i++) {
    guint16 k = g_array_index(kinds, guint16, i);
    if (i > 0) g_string_append(s, ", ");
    g_string_append_printf(s, "%u", k);
  }
  return g_string_free(s, FALSE);
}

static gchar *truncate_pubkey(const gchar *pk) {
  if (!pk) return g_strdup("Unknown");
  gsize len = strlen(pk);
  if (len > 16) {
    return g_strdup_printf("%.8s...%.8s", pk, pk + len - 8);
  }
  return g_strdup(pk);
}

/* Create a row for a delegation in the list */
static GtkWidget *create_delegation_row(const GnDelegation *d) {
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());

  /* Title: label or truncated delegatee */
  if (d->label && *d->label) {
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), d->label);
  } else {
    gchar *trunc = truncate_pubkey(d->delegatee_pubkey_hex);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), trunc);
    g_free(trunc);
  }

  /* Subtitle: kinds and validity */
  gchar *kinds = format_kinds(d->allowed_kinds);
  gchar *subtitle;
  if (d->valid_until > 0) {
    gchar *until = format_timestamp(d->valid_until);
    subtitle = g_strdup_printf("%s | Expires: %s", kinds, until);
    g_free(until);
  } else {
    subtitle = g_strdup_printf("%s | No expiry", kinds);
  }
  adw_action_row_set_subtitle(row, subtitle);
  g_free(subtitle);
  g_free(kinds);

  /* Status indicator */
  gboolean valid = gn_delegation_is_valid(d, 0, 0);
  GtkWidget *status = gtk_image_new();
  if (d->revoked) {
    gtk_image_set_from_icon_name(GTK_IMAGE(status), "action-unavailable-symbolic");
    gtk_widget_add_css_class(status, "error");
  } else if (!valid) {
    gtk_image_set_from_icon_name(GTK_IMAGE(status), "dialog-warning-symbolic");
    gtk_widget_add_css_class(status, "warning");
  } else {
    gtk_image_set_from_icon_name(GTK_IMAGE(status), "emblem-ok-symbolic");
    gtk_widget_add_css_class(status, "success");
  }
  adw_action_row_add_suffix(row, status);

  /* Arrow for navigation */
  GtkWidget *arrow = gtk_image_new_from_icon_name("go-next-symbolic");
  adw_action_row_add_suffix(row, arrow);

  /* Store delegation ID in row data */
  g_object_set_data_full(G_OBJECT(row), "delegation-id", g_strdup(d->id), g_free);

  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);

  return GTK_WIDGET(row);
}

static void populate_delegation_list(SheetDelegation *self) {
  if (!self->npub) return;

  /* Clear existing rows */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->list_delegations))) != NULL) {
    gtk_list_box_remove(self->list_delegations, child);
  }

  /* Load delegations */
  GPtrArray *delegations = gn_delegation_list(self->npub, TRUE);

  if (delegations->len == 0) {
    gtk_widget_set_visible(GTK_WIDGET(self->group_active), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->status_empty), TRUE);
  } else {
    gtk_widget_set_visible(GTK_WIDGET(self->group_active), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(self->status_empty), FALSE);

    for (guint i = 0; i < delegations->len; i++) {
      GnDelegation *d = g_ptr_array_index(delegations, i);
      GtkWidget *row = create_delegation_row(d);
      gtk_list_box_append(self->list_delegations, row);
    }
  }

  g_ptr_array_unref(delegations);
}

static void update_create_button_sensitivity(SheetDelegation *self) {
  const gchar *delegatee = gtk_editable_get_text(GTK_EDITABLE(self->entry_delegatee));

  gboolean valid = FALSE;
  if (delegatee && *delegatee) {
    gsize len = strlen(delegatee);
    /* Accept hex (64 chars) or npub */
    if (len == 64) {
      valid = TRUE;
      for (gsize i = 0; i < 64 && valid; i++) {
        if (!g_ascii_isxdigit(delegatee[i])) valid = FALSE;
      }
    } else if (g_str_has_prefix(delegatee, "npub1") && len >= 59) {
      valid = TRUE;
    }
  }

  gtk_widget_set_sensitive(self->btn_create, valid);
}

static void show_details_page(SheetDelegation *self, const gchar *delegation_id) {
  if (!self->npub || !delegation_id) return;

  GnDelegation *d = NULL;
  GnDelegationResult rc = gn_delegation_get(self->npub, delegation_id, &d);
  if (rc != GN_DELEGATION_OK) {
    g_warning("delegation: failed to get delegation %s: %s",
              delegation_id, gn_delegation_result_to_string(rc));
    return;
  }

  g_free(self->current_delegation_id);
  self->current_delegation_id = g_strdup(delegation_id);

  /* Populate details */
  gtk_label_set_text(self->lbl_detail_label, d->label ? d->label : "(no label)");

  gchar *trunc = truncate_pubkey(d->delegatee_pubkey_hex);
  gtk_label_set_text(self->lbl_detail_delegatee, trunc);
  g_free(trunc);

  gchar *kinds = format_kinds(d->allowed_kinds);
  gtk_label_set_text(self->lbl_detail_kinds, kinds);
  g_free(kinds);

  gchar *from = format_timestamp(d->valid_from);
  gtk_label_set_text(self->lbl_detail_from, d->valid_from > 0 ? from : "Immediate");
  g_free(from);

  gchar *until = format_timestamp(d->valid_until);
  gtk_label_set_text(self->lbl_detail_until, d->valid_until > 0 ? until : "Never");
  g_free(until);

  gchar *created = format_timestamp(d->created_at);
  gtk_label_set_text(self->lbl_detail_created, created);
  g_free(created);

  /* Status */
  if (d->revoked) {
    gchar *revoked_at = format_timestamp(d->revoked_at);
    gchar *status = g_strdup_printf("Revoked (%s)", revoked_at);
    gtk_label_set_text(self->lbl_detail_status, status);
    g_free(status);
    g_free(revoked_at);
    gtk_widget_set_sensitive(self->btn_revoke, FALSE);
  } else if (!gn_delegation_is_valid(d, 0, 0)) {
    gtk_label_set_text(self->lbl_detail_status, "Expired");
    gtk_widget_set_sensitive(self->btn_revoke, TRUE);
  } else {
    gtk_label_set_text(self->lbl_detail_status, "Active");
    gtk_widget_set_sensitive(self->btn_revoke, TRUE);
  }

  /* Delegation tag */
  gchar *tag = gn_delegation_build_tag(d);
  gtk_label_set_text(self->lbl_detail_tag, tag ? tag : "");
  g_free(tag);

  gn_delegation_free(d);

  gtk_stack_set_visible_child_name(GTK_STACK(self->stack_main), "details");
  gtk_widget_set_visible(GTK_WIDGET(self->btn_back), TRUE);
}

/* ======== Signal Handlers ======== */

static void on_create_new(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetDelegation *self = SHEET_DELEGATION(user_data);

  /* Reset form */
  gtk_editable_set_text(GTK_EDITABLE(self->entry_delegatee), "");
  gtk_editable_set_text(GTK_EDITABLE(self->entry_label), "");
  gtk_check_button_set_active(self->chk_all_kinds, TRUE);
  adw_expander_row_set_enable_expansion(self->expander_kinds, FALSE);
  gtk_check_button_set_active(self->chk_no_time_limit, TRUE);
  adw_expander_row_set_enable_expansion(self->expander_time, FALSE);

  /* Clear kind checkboxes */
  gtk_check_button_set_active(self->chk_kind_0, FALSE);
  gtk_check_button_set_active(self->chk_kind_1, FALSE);
  gtk_check_button_set_active(self->chk_kind_3, FALSE);
  gtk_check_button_set_active(self->chk_kind_4, FALSE);
  gtk_check_button_set_active(self->chk_kind_6, FALSE);
  gtk_check_button_set_active(self->chk_kind_7, FALSE);
  gtk_editable_set_text(GTK_EDITABLE(self->entry_custom_kind), "");

  /* Reset dropdowns */
  gtk_drop_down_set_selected(self->dropdown_from_preset, 0);
  gtk_drop_down_set_selected(self->dropdown_until_preset, 0);

  gtk_widget_set_sensitive(self->btn_create, FALSE);

  gtk_stack_set_visible_child_name(GTK_STACK(self->stack_main), "create");
  gtk_widget_set_visible(GTK_WIDGET(self->btn_back), TRUE);
}

static void on_back(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetDelegation *self = SHEET_DELEGATION(user_data);

  gtk_stack_set_visible_child_name(GTK_STACK(self->stack_main), "list");
  gtk_widget_set_visible(GTK_WIDGET(self->btn_back), FALSE);
}

static void on_close(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetDelegation *self = SHEET_DELEGATION(user_data);
  adw_dialog_close(ADW_DIALOG(self));
}

static void on_cancel_create(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetDelegation *self = SHEET_DELEGATION(user_data);

  gtk_stack_set_visible_child_name(GTK_STACK(self->stack_main), "list");
  gtk_widget_set_visible(GTK_WIDGET(self->btn_back), FALSE);
}

static void on_create(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetDelegation *self = SHEET_DELEGATION(user_data);

  if (!self->npub) return;

  /* Get delegatee pubkey */
  const gchar *delegatee_input = gtk_editable_get_text(GTK_EDITABLE(self->entry_delegatee));
  gchar *delegatee_hex = NULL;

  /* Convert npub to hex if needed */
  if (g_str_has_prefix(delegatee_input, "npub1")) {
    guint8 pk[32];
    if (nostr_nip19_decode_npub(delegatee_input, pk) == 0) {
      delegatee_hex = g_malloc(65);
      for (gint i = 0; i < 32; i++) {
        g_snprintf(delegatee_hex + i*2, 3, "%02x", pk[i]);
      }
    } else {
      GtkAlertDialog *ad = gtk_alert_dialog_new("Invalid npub format");
      gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
      g_object_unref(ad);
      return;
    }
  } else {
    delegatee_hex = g_strdup(delegatee_input);
  }

  /* Get label */
  const gchar *label = gtk_editable_get_text(GTK_EDITABLE(self->entry_label));
  if (label && !*label) label = NULL;

  /* Build allowed kinds array */
  GArray *allowed_kinds = NULL;
  if (!gtk_check_button_get_active(self->chk_all_kinds)) {
    allowed_kinds = g_array_new(FALSE, FALSE, sizeof(guint16));

    if (gtk_check_button_get_active(self->chk_kind_0)) {
      guint16 k = 0; g_array_append_val(allowed_kinds, k);
    }
    if (gtk_check_button_get_active(self->chk_kind_1)) {
      guint16 k = 1; g_array_append_val(allowed_kinds, k);
    }
    if (gtk_check_button_get_active(self->chk_kind_3)) {
      guint16 k = 3; g_array_append_val(allowed_kinds, k);
    }
    if (gtk_check_button_get_active(self->chk_kind_4)) {
      guint16 k = 4; g_array_append_val(allowed_kinds, k);
    }
    if (gtk_check_button_get_active(self->chk_kind_6)) {
      guint16 k = 6; g_array_append_val(allowed_kinds, k);
    }
    if (gtk_check_button_get_active(self->chk_kind_7)) {
      guint16 k = 7; g_array_append_val(allowed_kinds, k);
    }

    /* Custom kind */
    const gchar *custom = gtk_editable_get_text(GTK_EDITABLE(self->entry_custom_kind));
    if (custom && *custom) {
      guint16 k = (guint16)g_ascii_strtoull(custom, NULL, 10);
      if (k > 0) g_array_append_val(allowed_kinds, k);
    }
  }

  /* Build time constraints */
  gint64 valid_from = 0;
  gint64 valid_until = 0;

  if (!gtk_check_button_get_active(self->chk_no_time_limit)) {
    gint64 now = (gint64)time(NULL);

    guint from_idx = gtk_drop_down_get_selected(self->dropdown_from_preset);
    if (from_idx < G_N_ELEMENTS(from_presets) - 1) {
      valid_from = now + from_presets[from_idx].offset_seconds;
    }

    guint until_idx = gtk_drop_down_get_selected(self->dropdown_until_preset);
    if (until_idx < G_N_ELEMENTS(until_presets) - 1 && until_presets[until_idx].offset_seconds > 0) {
      valid_until = now + until_presets[until_idx].offset_seconds;
    }
  }

  /* Create delegation */
  GnDelegation *delegation = NULL;
  GnDelegationResult rc = gn_delegation_create(self->npub, delegatee_hex,
                                                allowed_kinds, valid_from, valid_until,
                                                label, &delegation);

  g_free(delegatee_hex);
  if (allowed_kinds) g_array_unref(allowed_kinds);

  if (rc != GN_DELEGATION_OK) {
    GtkAlertDialog *ad = gtk_alert_dialog_new("Failed to create delegation: %s",
        gn_delegation_result_to_string(rc));
    gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(ad);
    return;
  }

  /* Store created delegation */
  if (self->created_delegation) {
    gn_delegation_free(self->created_delegation);
  }
  self->created_delegation = delegation;

  /* Show result page */
  gchar *tag = gn_delegation_build_tag(delegation);
  gtk_label_set_text(self->lbl_delegation_tag, tag ? tag : "");
  g_free(tag);

  gchar *trunc = truncate_pubkey(delegation->delegatee_pubkey_hex);
  gtk_label_set_text(self->lbl_result_delegatee, trunc);
  g_free(trunc);

  gchar *kinds = format_kinds(delegation->allowed_kinds);
  gtk_label_set_text(self->lbl_result_kinds, kinds);
  g_free(kinds);

  if (valid_until > 0) {
    gchar *until = format_timestamp(valid_until);
    gchar *validity = g_strdup_printf("Expires: %s", until);
    gtk_label_set_text(self->lbl_result_validity, validity);
    g_free(validity);
    g_free(until);
  } else {
    gtk_label_set_text(self->lbl_result_validity, "No expiration");
  }

  gtk_stack_set_visible_child_name(GTK_STACK(self->stack_main), "result");

  /* Notify listener */
  if (self->on_changed) {
    self->on_changed(self->npub, self->on_changed_ud);
  }
}

static void copy_to_clipboard(SheetDelegation *self, const gchar *text) {
  GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(self));
  if (display) {
    GdkClipboard *clipboard = gdk_display_get_clipboard(display);
    if (clipboard) {
      gdk_clipboard_set_text(clipboard, text);

      GtkAlertDialog *ad = gtk_alert_dialog_new("Copied to clipboard!");
      gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
      g_object_unref(ad);
    }
  }
}

static void on_copy_tag(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetDelegation *self = SHEET_DELEGATION(user_data);

  const gchar *tag = gtk_label_get_text(self->lbl_delegation_tag);
  if (tag && *tag) {
    copy_to_clipboard(self, tag);
  }
}

static void on_done(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetDelegation *self = SHEET_DELEGATION(user_data);

  populate_delegation_list(self);
  gtk_stack_set_visible_child_name(GTK_STACK(self->stack_main), "list");
  gtk_widget_set_visible(GTK_WIDGET(self->btn_back), FALSE);
}

static void on_copy_detail_tag(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetDelegation *self = SHEET_DELEGATION(user_data);

  const gchar *tag = gtk_label_get_text(self->lbl_detail_tag);
  if (tag && *tag) {
    copy_to_clipboard(self, tag);
  }
}

static void on_back_to_list(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetDelegation *self = SHEET_DELEGATION(user_data);

  g_free(self->current_delegation_id);
  self->current_delegation_id = NULL;

  populate_delegation_list(self);
  gtk_stack_set_visible_child_name(GTK_STACK(self->stack_main), "list");
  gtk_widget_set_visible(GTK_WIDGET(self->btn_back), FALSE);
}

static void on_revoke(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetDelegation *self = SHEET_DELEGATION(user_data);

  if (!self->npub || !self->current_delegation_id) return;

  GnDelegationResult rc = gn_delegation_revoke(self->npub, self->current_delegation_id);
  if (rc != GN_DELEGATION_OK) {
    GtkAlertDialog *ad = gtk_alert_dialog_new("Failed to revoke delegation: %s",
        gn_delegation_result_to_string(rc));
    gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(ad);
    return;
  }

  /* Update the details view */
  show_details_page(self, self->current_delegation_id);

  /* Notify listener */
  if (self->on_changed) {
    self->on_changed(self->npub, self->on_changed_ud);
  }
}

static void on_delegatee_changed(GtkEditable *editable, gpointer user_data) {
  (void)editable;
  SheetDelegation *self = SHEET_DELEGATION(user_data);
  update_create_button_sensitivity(self);
}

static void on_all_kinds_toggled(GtkCheckButton *btn, gpointer user_data) {
  SheetDelegation *self = SHEET_DELEGATION(user_data);
  gboolean all_kinds = gtk_check_button_get_active(btn);

  /* Disable specific kinds when "all kinds" is checked */
  adw_expander_row_set_enable_expansion(self->expander_kinds, !all_kinds);
}

static void on_no_time_limit_toggled(GtkCheckButton *btn, gpointer user_data) {
  SheetDelegation *self = SHEET_DELEGATION(user_data);
  gboolean no_limit = gtk_check_button_get_active(btn);

  /* Disable time settings when "no limit" is checked */
  adw_expander_row_set_enable_expansion(self->expander_time, !no_limit);
}

static void on_delegation_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
  (void)box;
  SheetDelegation *self = SHEET_DELEGATION(user_data);

  const gchar *id = g_object_get_data(G_OBJECT(row), "delegation-id");
  if (id) {
    show_details_page(self, id);
  }
}
