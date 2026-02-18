/* sheet-multisig-signing.c - Multi-signature signing progress dialog implementation
 *
 * Displays real-time signing progress during a multi-signature operation.
 *
 * Issue: nostrc-orz
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sheet-multisig-signing.h"
#include "../app-resources.h"
#include "../../multisig_wallet.h"
#include "../../multisig_coordinator.h"
#include "../../accounts_store.h"
#include "../../secure-memory.h"

#include <gtk/gtk.h>
#include <adwaita.h>
#include <string.h>

struct _SheetMultisigSigning {
  AdwDialog parent_instance;

  /* Header */
  GtkLabel *lbl_title;
  GtkLabel *lbl_wallet_name;
  GtkLabel *lbl_event_kind;

  /* Progress indicator */
  GtkProgressBar *progress_bar;
  GtkLabel *lbl_progress;

  /* Signer list */
  GtkListBox *list_signers;

  /* Event preview */
  GtkLabel *lbl_event_preview;
  AdwExpanderRow *expander_event;

  /* Status banners */
  AdwBanner *banner_waiting;
  AdwBanner *banner_success;
  AdwBanner *banner_error;

  /* Action buttons */
  GtkButton *btn_cancel;
  GtkButton *btn_close;
  GtkSpinner *spinner;

  /* State */
  gchar *wallet_id;
  gchar *event_json;
  gchar *session_id;
  gchar *final_signature;
  guint signatures_collected;
  guint signatures_required;
  gboolean is_complete;
  gboolean was_canceled;

  /* Signer tracking */
  GHashTable *signer_rows;  /* npub -> GtkWidget* (row) */

  /* Callback */
  SheetMultisigSigningCallback callback;
  gpointer callback_data;
};

G_DEFINE_TYPE(SheetMultisigSigning, sheet_multisig_signing, ADW_TYPE_DIALOG)

/* Forward declarations */
static void update_progress_display(SheetMultisigSigning *self);
static void populate_signer_list(SheetMultisigSigning *self);
static void update_signer_row(SheetMultisigSigning *self,
                              const gchar *npub,
                              CosignerStatus status);
static void on_coordinator_progress(const gchar *session_id,
                                    guint collected,
                                    guint required,
                                    const gchar *latest_signer,
                                    gpointer user_data);
static void on_coordinator_complete(const gchar *session_id,
                                    gboolean success,
                                    const gchar *signature,
                                    const gchar *error,
                                    gpointer user_data);

/* ======== Helpers ======== */

static GtkWidget *create_signer_status_row(const gchar *label,
                                           const gchar *npub,
                                           CosignerType type,
                                           CosignerStatus status) {
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), label ? label : "Signer");

  /* Subtitle shows type */
  adw_action_row_set_subtitle(row, type == COSIGNER_TYPE_LOCAL ? "Local" : "Remote (NIP-46)");

  /* Type icon */
  GtkWidget *type_icon = gtk_image_new_from_icon_name(
      type == COSIGNER_TYPE_LOCAL ? "computer-symbolic" : "network-server-symbolic");
  adw_action_row_add_prefix(row, type_icon);

  /* Status icon/spinner */
  GtkWidget *status_widget;
  switch (status) {
    case COSIGNER_STATUS_PENDING:
    case COSIGNER_STATUS_REQUESTED:
      status_widget = gtk_spinner_new();
      gtk_spinner_start(GTK_SPINNER(status_widget));
      break;
    case COSIGNER_STATUS_SIGNED:
      status_widget = gtk_image_new_from_icon_name("emblem-ok-symbolic");
      gtk_widget_add_css_class(status_widget, "success");
      break;
    case COSIGNER_STATUS_REJECTED:
      status_widget = gtk_image_new_from_icon_name("dialog-error-symbolic");
      gtk_widget_add_css_class(status_widget, "error");
      break;
    case COSIGNER_STATUS_TIMEOUT:
      status_widget = gtk_image_new_from_icon_name("alarm-symbolic");
      gtk_widget_add_css_class(status_widget, "warning");
      break;
    case COSIGNER_STATUS_ERROR:
    default:
      status_widget = gtk_image_new_from_icon_name("dialog-warning-symbolic");
      gtk_widget_add_css_class(status_widget, "warning");
      break;
  }
  adw_action_row_add_suffix(row, status_widget);

  /* Store status widget reference */
  g_object_set_data(G_OBJECT(row), "status_widget", status_widget);
  g_object_set_data_full(G_OBJECT(row), "npub", g_strdup(npub), g_free);

  return GTK_WIDGET(row);
}

static const gchar *get_event_kind_name(gint kind) {
  switch (kind) {
    case 0: return "Profile Metadata";
    case 1: return "Text Note";
    case 2: return "Relay List";
    case 3: return "Contacts";
    case 4: return "Encrypted Direct Message";
    case 5: return "Event Deletion";
    case 6: return "Repost";
    case 7: return "Reaction";
    case 40: return "Channel Creation";
    case 41: return "Channel Metadata";
    case 42: return "Channel Message";
    case 43: return "Channel Hide Message";
    case 44: return "Channel Mute User";
    case 1984: return "Report";
    case 9734: return "Zap Request";
    case 9735: return "Zap";
    case 10000: return "Mute List";
    case 10001: return "Pin List";
    case 10002: return "Relay List Metadata";
    case 30000: return "Categorized People List";
    case 30001: return "Categorized Bookmark List";
    case 30023: return "Long-form Content";
    default:
      if (kind >= 10000 && kind < 20000)
        return "Replaceable Event";
      if (kind >= 20000 && kind < 30000)
        return "Ephemeral Event";
      if (kind >= 30000 && kind < 40000)
        return "Parameterized Replaceable Event";
      return "Unknown Event";
  }
}

/* ======== Progress Updates ======== */

static void update_progress_display(SheetMultisigSigning *self) {
  if (!self) return;

  /* Update progress bar */
  if (self->progress_bar && self->signatures_required > 0) {
    gdouble fraction = (gdouble)self->signatures_collected / (gdouble)self->signatures_required;
    gtk_progress_bar_set_fraction(self->progress_bar, MIN(fraction, 1.0));
  }

  /* Update progress label */
  if (self->lbl_progress) {
    gchar *progress_text = multisig_format_progress(self->signatures_collected,
                                                     self->signatures_required);
    gtk_label_set_text(self->lbl_progress, progress_text);
    g_free(progress_text);
  }
}

static void populate_signer_list(SheetMultisigSigning *self) {
  if (!self || !self->list_signers || !self->wallet_id) return;

  /* Clear existing rows */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->list_signers))) != NULL) {
    gtk_list_box_remove(self->list_signers, child);
  }

  if (self->signer_rows) {
    g_hash_table_remove_all(self->signer_rows);
  }

  /* Get wallet info */
  MultisigWallet *wallet = NULL;
  if (multisig_wallet_get(self->wallet_id, &wallet) != MULTISIG_OK || !wallet) {
    return;
  }

  AccountsStore *as = accounts_store_get_default();

  /* Add rows for each co-signer */
  for (guint i = 0; i < wallet->cosigners->len; i++) {
    MultisigCosigner *cs = g_ptr_array_index(wallet->cosigners, i);

    g_autofree gchar *label = NULL;
    if (cs->label && *cs->label) {
      label = g_strdup(cs->label);
    } else if (cs->type == COSIGNER_TYPE_LOCAL) {
      label = accounts_store_get_display_name(as, cs->npub);
    } else {
      label = g_strdup_printf("Remote Signer %u", i + 1);
    }

    GtkWidget *row = create_signer_status_row(label, cs->npub, cs->type,
                                               COSIGNER_STATUS_PENDING);

    gtk_list_box_append(self->list_signers, row);

    if (self->signer_rows && cs->npub) {
      g_hash_table_insert(self->signer_rows, g_strdup(cs->npub), row);
    }
  }

  self->signatures_required = wallet->threshold_m;
  multisig_wallet_free(wallet);

  update_progress_display(self);
}

static void update_signer_row(SheetMultisigSigning *self,
                              const gchar *npub,
                              CosignerStatus status) {
  if (!self || !npub || !self->signer_rows) return;

  GtkWidget *row = g_hash_table_lookup(self->signer_rows, npub);
  if (!row) return;

  /* Get the old status widget */
  GtkWidget *old_widget = g_object_get_data(G_OBJECT(row), "status_widget");

  /* Create new status widget */
  GtkWidget *new_widget;
  switch (status) {
    case COSIGNER_STATUS_PENDING:
    case COSIGNER_STATUS_REQUESTED:
      new_widget = gtk_spinner_new();
      gtk_spinner_start(GTK_SPINNER(new_widget));
      break;
    case COSIGNER_STATUS_SIGNED:
      new_widget = gtk_image_new_from_icon_name("emblem-ok-symbolic");
      gtk_widget_add_css_class(new_widget, "success");
      break;
    case COSIGNER_STATUS_REJECTED:
      new_widget = gtk_image_new_from_icon_name("dialog-error-symbolic");
      gtk_widget_add_css_class(new_widget, "error");
      break;
    case COSIGNER_STATUS_TIMEOUT:
      new_widget = gtk_image_new_from_icon_name("alarm-symbolic");
      gtk_widget_add_css_class(new_widget, "warning");
      break;
    case COSIGNER_STATUS_ERROR:
    default:
      new_widget = gtk_image_new_from_icon_name("dialog-warning-symbolic");
      gtk_widget_add_css_class(new_widget, "warning");
      break;
  }

  /* Replace the widget */
  if (old_widget && ADW_IS_ACTION_ROW(row)) {
    adw_action_row_remove(ADW_ACTION_ROW(row), old_widget);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), new_widget);
    g_object_set_data(G_OBJECT(row), "status_widget", new_widget);
  }
}

/* ======== Coordinator Callbacks ======== */

static void on_coordinator_progress(const gchar *session_id,
                                    guint collected,
                                    guint required,
                                    const gchar *latest_signer,
                                    gpointer user_data) {
  SheetMultisigSigning *self = SHEET_MULTISIG_SIGNING(user_data);
  if (!self || g_strcmp0(session_id, self->session_id) != 0) return;

  self->signatures_collected = collected;
  self->signatures_required = required;

  /* Update signer row */
  if (latest_signer) {
    update_signer_row(self, latest_signer, COSIGNER_STATUS_SIGNED);
  }

  update_progress_display(self);
}

static void on_coordinator_complete(const gchar *session_id,
                                    gboolean success,
                                    const gchar *signature,
                                    const gchar *error,
                                    gpointer user_data) {
  SheetMultisigSigning *self = SHEET_MULTISIG_SIGNING(user_data);
  if (!self || g_strcmp0(session_id, self->session_id) != 0) return;

  sheet_multisig_signing_complete(self, success, signature, error);
}

/* ======== Button Handlers ======== */

static void on_cancel_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetMultisigSigning *self = SHEET_MULTISIG_SIGNING(user_data);
  if (!self) return;

  self->was_canceled = TRUE;

  /* Cancel the signing session */
  if (self->session_id) {
    MultisigCoordinator *coord = multisig_coordinator_get_default();
    multisig_coordinator_cancel_session(coord, self->session_id);
  }

  /* Notify callback */
  if (self->callback) {
    self->callback(FALSE, NULL, self->callback_data);
  }

  /* Close dialog */
  adw_dialog_close(ADW_DIALOG(self));
}

static void on_close_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetMultisigSigning *self = SHEET_MULTISIG_SIGNING(user_data);
  if (!self) return;

  /* Notify callback with result */
  if (self->callback) {
    self->callback(self->is_complete && !self->was_canceled,
                   self->final_signature,
                   self->callback_data);
  }

  adw_dialog_close(ADW_DIALOG(self));
}

/* ======== Public API ======== */

void sheet_multisig_signing_start(SheetMultisigSigning *self) {
  g_return_if_fail(SHEET_IS_MULTISIG_SIGNING(self));
  g_return_if_fail(self->wallet_id != NULL);
  g_return_if_fail(self->event_json != NULL);

  /* Show waiting banner */
  if (self->banner_waiting) {
    adw_banner_set_revealed(self->banner_waiting, TRUE);
  }
  if (self->banner_success) {
    adw_banner_set_revealed(self->banner_success, FALSE);
  }
  if (self->banner_error) {
    adw_banner_set_revealed(self->banner_error, FALSE);
  }

  /* Show spinner */
  if (self->spinner) {
    gtk_spinner_start(self->spinner);
    gtk_widget_set_visible(GTK_WIDGET(self->spinner), TRUE);
  }

  /* Enable cancel, disable close */
  if (self->btn_cancel) {
    gtk_widget_set_visible(GTK_WIDGET(self->btn_cancel), TRUE);
  }
  if (self->btn_close) {
    gtk_widget_set_visible(GTK_WIDGET(self->btn_close), FALSE);
  }

  /* Start signing via coordinator */
  MultisigCoordinator *coord = multisig_coordinator_get_default();
  GError *error = NULL;

  gboolean started = multisig_coordinator_start_signing(
      coord,
      self->wallet_id,
      self->event_json,
      TRUE,  /* auto-sign local */
      on_coordinator_progress,
      on_coordinator_complete,
      self,
      &self->session_id,
      &error);

  if (!started) {
    sheet_multisig_signing_complete(self, FALSE, NULL,
                                    error ? error->message : "Failed to start signing");
    g_clear_error(&error);
  }
}

void sheet_multisig_signing_update_progress(SheetMultisigSigning *self,
                                            const gchar *signer_npub,
                                            CosignerStatus status) {
  g_return_if_fail(SHEET_IS_MULTISIG_SIGNING(self));

  if (status == COSIGNER_STATUS_SIGNED) {
    self->signatures_collected++;
  }

  update_signer_row(self, signer_npub, status);
  update_progress_display(self);
}

void sheet_multisig_signing_complete(SheetMultisigSigning *self,
                                     gboolean success,
                                     const gchar *signature,
                                     const gchar *error_message) {
  g_return_if_fail(SHEET_IS_MULTISIG_SIGNING(self));

  self->is_complete = TRUE;

  /* Stop spinner */
  if (self->spinner) {
    gtk_spinner_stop(self->spinner);
    gtk_widget_set_visible(GTK_WIDGET(self->spinner), FALSE);
  }

  /* Update progress to full if success */
  if (success && self->progress_bar) {
    gtk_progress_bar_set_fraction(self->progress_bar, 1.0);
  }

  /* Update banners */
  if (self->banner_waiting) {
    adw_banner_set_revealed(self->banner_waiting, FALSE);
  }

  if (success) {
    /* Store signature */
    if (signature) {
      g_free(self->final_signature);
      self->final_signature = gn_secure_strdup(signature);
    }

    if (self->banner_success) {
      adw_banner_set_title(self->banner_success,
                           "Signing complete! All required signatures collected.");
      adw_banner_set_revealed(self->banner_success, TRUE);
    }

    if (self->lbl_progress) {
      gtk_label_set_text(self->lbl_progress, "Signing complete!");
      gtk_widget_add_css_class(GTK_WIDGET(self->lbl_progress), "success");
    }
  } else {
    if (self->banner_error) {
      adw_banner_set_title(self->banner_error,
                           error_message ? error_message : "Signing failed");
      adw_banner_set_revealed(self->banner_error, TRUE);
    }

    if (self->lbl_progress) {
      gtk_label_set_text(self->lbl_progress, "Signing failed");
      gtk_widget_add_css_class(GTK_WIDGET(self->lbl_progress), "error");
    }
  }

  /* Show close button, hide cancel */
  if (self->btn_cancel) {
    gtk_widget_set_visible(GTK_WIDGET(self->btn_cancel), FALSE);
  }
  if (self->btn_close) {
    gtk_widget_set_visible(GTK_WIDGET(self->btn_close), TRUE);
    if (success) {
      gtk_widget_add_css_class(GTK_WIDGET(self->btn_close), "suggested-action");
    }
  }
}

/* ======== Lifecycle ======== */

static void sheet_multisig_signing_dispose(GObject *object) {
  SheetMultisigSigning *self = SHEET_MULTISIG_SIGNING(object);

  g_free(self->wallet_id);
  g_free(self->event_json);
  g_free(self->session_id);

  if (self->final_signature) {
    gn_secure_strfree(self->final_signature);
    self->final_signature = NULL;
  }

  g_clear_pointer(&self->signer_rows, g_hash_table_destroy);

  G_OBJECT_CLASS(sheet_multisig_signing_parent_class)->dispose(object);
}

static void sheet_multisig_signing_class_init(SheetMultisigSigningClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = sheet_multisig_signing_dispose;

  /* In a full implementation, load from .ui file */
  /* gtk_widget_class_set_template_from_resource(widget_class,
   *     APP_RESOURCE_PATH "/ui/sheets/sheet-multisig-signing.ui"); */
}

static void sheet_multisig_signing_init(SheetMultisigSigning *self) {
  self->signer_rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  /* Build UI programmatically */
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(content, 24);
  gtk_widget_set_margin_end(content, 24);
  gtk_widget_set_margin_top(content, 24);
  gtk_widget_set_margin_bottom(content, 24);

  /* Title */
  self->lbl_title = GTK_LABEL(gtk_label_new("Multi-Signature Signing"));
  gtk_widget_add_css_class(GTK_WIDGET(self->lbl_title), "title-1");
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(self->lbl_title));

  /* Wallet name */
  self->lbl_wallet_name = GTK_LABEL(gtk_label_new(""));
  gtk_widget_add_css_class(GTK_WIDGET(self->lbl_wallet_name), "dim-label");
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(self->lbl_wallet_name));

  /* Waiting banner */
  self->banner_waiting = ADW_BANNER(adw_banner_new("Collecting signatures from co-signers..."));
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(self->banner_waiting));

  /* Success banner */
  self->banner_success = ADW_BANNER(adw_banner_new(""));
  gtk_widget_add_css_class(GTK_WIDGET(self->banner_success), "success");
  adw_banner_set_revealed(self->banner_success, FALSE);
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(self->banner_success));

  /* Error banner */
  self->banner_error = ADW_BANNER(adw_banner_new(""));
  gtk_widget_add_css_class(GTK_WIDGET(self->banner_error), "error");
  adw_banner_set_revealed(self->banner_error, FALSE);
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(self->banner_error));

  /* Progress section */
  GtkWidget *progress_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(progress_box, 12);
  gtk_widget_set_margin_bottom(progress_box, 12);

  self->progress_bar = GTK_PROGRESS_BAR(gtk_progress_bar_new());
  gtk_box_append(GTK_BOX(progress_box), GTK_WIDGET(self->progress_bar));

  self->lbl_progress = GTK_LABEL(gtk_label_new("0 of 0 signatures collected"));
  gtk_box_append(GTK_BOX(progress_box), GTK_WIDGET(self->lbl_progress));

  gtk_box_append(GTK_BOX(content), progress_box);

  /* Signers list */
  AdwPreferencesGroup *group_signers = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(group_signers, "Co-Signers");

  self->list_signers = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(self->list_signers, GTK_SELECTION_NONE);
  gtk_widget_add_css_class(GTK_WIDGET(self->list_signers), "boxed-list");
  adw_preferences_group_add(group_signers, GTK_WIDGET(self->list_signers));

  gtk_box_append(GTK_BOX(content), GTK_WIDGET(group_signers));

  /* Event kind */
  self->lbl_event_kind = GTK_LABEL(gtk_label_new(""));
  gtk_widget_add_css_class(GTK_WIDGET(self->lbl_event_kind), "dim-label");
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(self->lbl_event_kind));

  /* Buttons */
  GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_halign(button_box, GTK_ALIGN_END);
  gtk_widget_set_margin_top(button_box, 12);

  self->spinner = GTK_SPINNER(gtk_spinner_new());
  gtk_box_append(GTK_BOX(button_box), GTK_WIDGET(self->spinner));

  self->btn_cancel = GTK_BUTTON(gtk_button_new_with_label("Cancel"));
  gtk_widget_add_css_class(GTK_WIDGET(self->btn_cancel), "destructive-action");
  g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel_clicked), self);
  gtk_box_append(GTK_BOX(button_box), GTK_WIDGET(self->btn_cancel));

  self->btn_close = GTK_BUTTON(gtk_button_new_with_label("Close"));
  gtk_widget_set_visible(GTK_WIDGET(self->btn_close), FALSE);
  g_signal_connect(self->btn_close, "clicked", G_CALLBACK(on_close_clicked), self);
  gtk_box_append(GTK_BOX(button_box), GTK_WIDGET(self->btn_close));

  gtk_box_append(GTK_BOX(content), button_box);

  /* Set as dialog child */
  adw_dialog_set_child(ADW_DIALOG(self), content);
  adw_dialog_set_title(ADW_DIALOG(self), "Signing in Progress");
}

SheetMultisigSigning *sheet_multisig_signing_new(const gchar *wallet_id,
                                                  const gchar *event_json) {
  SheetMultisigSigning *self = g_object_new(SHEET_TYPE_MULTISIG_SIGNING, NULL);

  self->wallet_id = g_strdup(wallet_id);
  self->event_json = g_strdup(event_json);

  /* Get wallet info for display */
  MultisigWallet *wallet = NULL;
  if (multisig_wallet_get(wallet_id, &wallet) == MULTISIG_OK && wallet) {
    if (self->lbl_wallet_name) {
      g_autofree gchar *subtitle = g_strdup_printf("Wallet: %s (%u-of-%u)",
                                        wallet->name,
                                        wallet->threshold_m,
                                        wallet->total_n);
      gtk_label_set_text(self->lbl_wallet_name, subtitle);
    }
    multisig_wallet_free(wallet);
  }

  /* Extract event kind for display */
  if (event_json) {
    const gchar *kind_str = strstr(event_json, "\"kind\"");
    if (kind_str) {
      kind_str = strchr(kind_str, ':');
      if (kind_str) {
        kind_str++;
        while (*kind_str == ' ') kind_str++;
        gint kind = (gint)g_ascii_strtoll(kind_str, NULL, 10);

        if (self->lbl_event_kind) {
          g_autofree gchar *kind_text = g_strdup_printf("Event type: %s (kind %d)",
                                              get_event_kind_name(kind), kind);
          gtk_label_set_text(self->lbl_event_kind, kind_text);
        }
      }
    }
  }

  /* Populate signer list */
  populate_signer_list(self);

  return self;
}

void sheet_multisig_signing_set_callback(SheetMultisigSigning *self,
                                         SheetMultisigSigningCallback callback,
                                         gpointer user_data) {
  g_return_if_fail(SHEET_IS_MULTISIG_SIGNING(self));
  self->callback = callback;
  self->callback_data = user_data;
}
