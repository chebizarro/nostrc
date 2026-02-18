/* sheet-create-multisig.c - Multi-signature wallet creation dialog implementation
 *
 * Multi-step wizard for creating a new multisig wallet configuration.
 *
 * Issue: nostrc-orz
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sheet-create-multisig.h"
#include "../app-resources.h"
#include "../../multisig_wallet.h"
#include "../../accounts_store.h"
#include "../../secret_store.h"

#include <gtk/gtk.h>
#include <adwaita.h>
#include <string.h>

/* Step identifiers */
#define STEP_THRESHOLD    "step-threshold"
#define STEP_LOCAL        "step-local"
#define STEP_REMOTE       "step-remote"
#define STEP_REVIEW       "step-review"
#define STEP_SUCCESS      "step-success"

/* Maximum signers */
#define MAX_SIGNERS 10

struct _SheetCreateMultisig {
  AdwDialog parent_instance;

  /* Navigation */
  AdwNavigationView *nav_view;

  /* Step 1: Threshold configuration */
  GtkSpinButton *spin_threshold_m;
  GtkSpinButton *spin_threshold_n;
  AdwEntryRow *entry_wallet_name;
  GtkLabel *lbl_threshold_summary;
  GtkButton *btn_step1_next;

  /* Step 2: Local signers */
  GtkListBox *list_local_signers;
  GtkLabel *lbl_local_count;
  GtkButton *btn_step2_back;
  GtkButton *btn_step2_next;

  /* Step 3: Remote signers */
  AdwEntryRow *entry_bunker_uri;
  GtkListBox *list_remote_signers;
  GtkLabel *lbl_remote_count;
  GtkButton *btn_add_remote;
  GtkButton *btn_step3_back;
  GtkButton *btn_step3_next;

  /* Step 4: Review */
  GtkLabel *lbl_review_name;
  GtkLabel *lbl_review_threshold;
  GtkLabel *lbl_review_signers;
  GtkListBox *list_review_signers;
  GtkButton *btn_step4_back;
  GtkButton *btn_step4_create;
  GtkSpinner *spinner_creating;

  /* Step 5: Success */
  GtkLabel *lbl_wallet_id;
  AdwBanner *banner_success;
  GtkButton *btn_finish;

  /* State */
  gchar *wallet_name;
  guint threshold_m;
  guint threshold_n;
  GPtrArray *selected_local;  /* npub strings */
  GPtrArray *remote_uris;     /* bunker URI strings */
  gchar *created_wallet_id;

  /* Callback */
  SheetCreateMultisigCallback on_created;
  gpointer on_created_data;
};

G_DEFINE_TYPE(SheetCreateMultisig, sheet_create_multisig, ADW_TYPE_DIALOG)

/* Forward declarations */
static void go_to_step(SheetCreateMultisig *self, const gchar *step);
static void update_threshold_summary(SheetCreateMultisig *self);
static void populate_local_signers(SheetCreateMultisig *self);
static void update_local_count(SheetCreateMultisig *self);
static void update_remote_count(SheetCreateMultisig *self);
static void populate_review(SheetCreateMultisig *self);
static gboolean create_wallet(SheetCreateMultisig *self);
static void validate_step1(SheetCreateMultisig *self);
static void validate_step2(SheetCreateMultisig *self);
static void validate_step3(SheetCreateMultisig *self);
static void on_local_signer_toggled(GtkCheckButton *check, gpointer user_data);
static void on_remove_remote_clicked(GtkButton *btn, gpointer user_data);
static void update_remote_list(SheetCreateMultisig *self);

/* ======== Helpers ======== */

static void clear_list_box(GtkListBox *list_box) {
  if (!list_box) return;
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(list_box))) != NULL) {
    gtk_list_box_remove(list_box, child);
  }
}

static GtkWidget *create_signer_row(const gchar *label, const gchar *npub,
                                    gboolean with_checkbox, gboolean checked) {
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), label ? label : "");

  /* Show truncated npub as subtitle */
  if (npub && strlen(npub) > 20) {
    gchar *truncated = g_strdup_printf("%.12s...%.6s", npub, npub + strlen(npub) - 6);
    adw_action_row_set_subtitle(row, truncated);
    g_free(truncated);
  } else if (npub) {
    adw_action_row_set_subtitle(row, npub);
  }

  if (with_checkbox) {
    GtkWidget *check = gtk_check_button_new();
    gtk_check_button_set_active(GTK_CHECK_BUTTON(check), checked);
    adw_action_row_add_prefix(row, check);
    adw_action_row_set_activatable_widget(row, check);

    /* Store npub as object data for retrieval */
    g_object_set_data_full(G_OBJECT(row), "npub", g_strdup(npub), g_free);
    g_object_set_data(G_OBJECT(row), "checkbox", check);
  }

  return GTK_WIDGET(row);
}

static GtkWidget *create_remote_signer_row(const gchar *bunker_uri, const gchar *label) {
  AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                label ? label : "Remote Signer");

  /* Show truncated bunker URI as subtitle */
  if (bunker_uri && strlen(bunker_uri) > 40) {
    gchar *truncated = g_strdup_printf("%.30s...", bunker_uri);
    adw_action_row_set_subtitle(row, truncated);
    g_free(truncated);
  } else if (bunker_uri) {
    adw_action_row_set_subtitle(row, bunker_uri);
  }

  /* Add remove button */
  GtkWidget *btn_remove = gtk_button_new_from_icon_name("user-trash-symbolic");
  gtk_widget_add_css_class(btn_remove, "flat");
  gtk_widget_set_valign(btn_remove, GTK_ALIGN_CENTER);
  adw_action_row_add_suffix(row, btn_remove);

  /* Store URI as object data */
  g_object_set_data_full(G_OBJECT(row), "bunker_uri", g_strdup(bunker_uri), g_free);
  g_object_set_data(G_OBJECT(row), "remove_button", btn_remove);

  return GTK_WIDGET(row);
}

/* ======== Navigation ======== */

static void go_to_step(SheetCreateMultisig *self, const gchar *step) {
  if (!self || !step) return;

  /* Find or create the appropriate page */
  AdwNavigationPage *page = NULL;

  if (g_strcmp0(step, STEP_THRESHOLD) == 0) {
    /* First page, already in nav_view */
    return;
  } else if (g_strcmp0(step, STEP_LOCAL) == 0) {
    populate_local_signers(self);
    page = ADW_NAVIGATION_PAGE(adw_navigation_page_new(
        GTK_WIDGET(gtk_builder_get_object(NULL, "step_local_content")),
        "Select Local Signers"));
  } else if (g_strcmp0(step, STEP_REMOTE) == 0) {
    page = ADW_NAVIGATION_PAGE(adw_navigation_page_new(
        GTK_WIDGET(gtk_builder_get_object(NULL, "step_remote_content")),
        "Add Remote Signers"));
  } else if (g_strcmp0(step, STEP_REVIEW) == 0) {
    populate_review(self);
    page = ADW_NAVIGATION_PAGE(adw_navigation_page_new(
        GTK_WIDGET(gtk_builder_get_object(NULL, "step_review_content")),
        "Review Configuration"));
  } else if (g_strcmp0(step, STEP_SUCCESS) == 0) {
    page = ADW_NAVIGATION_PAGE(adw_navigation_page_new(
        GTK_WIDGET(gtk_builder_get_object(NULL, "step_success_content")),
        "Wallet Created"));
  }

  /* For now, we use a simple approach - just update visibility */
  /* A full implementation would use AdwNavigationView properly */
}

/* ======== Step 1: Threshold ======== */

static void update_threshold_summary(SheetCreateMultisig *self) {
  if (!self || !self->lbl_threshold_summary) return;

  guint m = (guint)gtk_spin_button_get_value_as_int(self->spin_threshold_m);
  guint n = (guint)gtk_spin_button_get_value_as_int(self->spin_threshold_n);

  gchar *summary = g_strdup_printf(
      "This wallet will require %u of %u signatures to sign transactions.",
      m, n);
  gtk_label_set_text(self->lbl_threshold_summary, summary);
  g_free(summary);

  self->threshold_m = m;
  self->threshold_n = n;

  validate_step1(self);
}

static void on_threshold_changed(GtkSpinButton *spin, gpointer user_data) {
  (void)spin;
  SheetCreateMultisig *self = SHEET_CREATE_MULTISIG(user_data);

  /* Ensure m <= n */
  guint m = (guint)gtk_spin_button_get_value_as_int(self->spin_threshold_m);
  guint n = (guint)gtk_spin_button_get_value_as_int(self->spin_threshold_n);

  if (m > n) {
    gtk_spin_button_set_value(self->spin_threshold_m, (gdouble)n);
  }

  /* Update N's minimum to be >= M */
  GtkAdjustment *adj_n = gtk_spin_button_get_adjustment(self->spin_threshold_n);
  gtk_adjustment_set_lower(adj_n, (gdouble)m);

  update_threshold_summary(self);
}

static void validate_step1(SheetCreateMultisig *self) {
  if (!self) return;

  const gchar *name = gtk_editable_get_text(GTK_EDITABLE(self->entry_wallet_name));
  gboolean valid = name && *name && self->threshold_m >= 1 &&
                   self->threshold_n >= self->threshold_m;

  if (self->btn_step1_next) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_step1_next), valid);
  }
}

static void on_wallet_name_changed(GtkEditable *editable, gpointer user_data) {
  (void)editable;
  SheetCreateMultisig *self = SHEET_CREATE_MULTISIG(user_data);
  validate_step1(self);
}

static void on_step1_next(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetCreateMultisig *self = SHEET_CREATE_MULTISIG(user_data);
  if (!self) return;

  /* Save configuration */
  g_free(self->wallet_name);
  self->wallet_name = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->entry_wallet_name)));
  self->threshold_m = (guint)gtk_spin_button_get_value_as_int(self->spin_threshold_m);
  self->threshold_n = (guint)gtk_spin_button_get_value_as_int(self->spin_threshold_n);

  /* Show local signers step */
  populate_local_signers(self);
  go_to_step(self, STEP_LOCAL);
}

/* ======== Step 2: Local Signers ======== */

static void populate_local_signers(SheetCreateMultisig *self) {
  if (!self || !self->list_local_signers) return;

  clear_list_box(self->list_local_signers);

  /* Get all accounts */
  AccountsStore *as = accounts_store_get_default();
  GPtrArray *accounts = accounts_store_list(as);

  if (!accounts || accounts->len == 0) {
    /* No accounts - show message */
    GtkWidget *label = gtk_label_new("No local accounts available.\n"
                                     "Create an account first, or skip to add remote signers.");
    gtk_widget_set_margin_top(label, 20);
    gtk_widget_set_margin_bottom(label, 20);
    gtk_widget_add_css_class(label, "dim-label");
    gtk_list_box_append(self->list_local_signers, label);
  } else {
    for (guint i = 0; i < accounts->len; i++) {
      AccountEntry *entry = g_ptr_array_index(accounts, i);

      /* Skip watch-only accounts */
      if (entry->watch_only) continue;

      /* Check if already selected */
      gboolean selected = FALSE;
      for (guint j = 0; j < self->selected_local->len; j++) {
        if (g_strcmp0(entry->id, g_ptr_array_index(self->selected_local, j)) == 0) {
          selected = TRUE;
          break;
        }
      }

      GtkWidget *row = create_signer_row(
          entry->label && *entry->label ? entry->label : "Unnamed",
          entry->id,
          TRUE,
          selected);

      /* Connect checkbox toggle */
      GtkWidget *check = g_object_get_data(G_OBJECT(row), "checkbox");
      if (check) {
        g_object_set_data(G_OBJECT(check), "sheet", self);
        g_object_set_data_full(G_OBJECT(check), "npub", g_strdup(entry->id), g_free);
        g_signal_connect(check, "toggled",
                         G_CALLBACK(on_local_signer_toggled), self);
      }

      gtk_list_box_append(self->list_local_signers, row);
    }
  }

  if (accounts) {
    g_ptr_array_unref(accounts);
  }

  update_local_count(self);
}

static void on_local_signer_toggled(GtkCheckButton *check, gpointer user_data) {
  SheetCreateMultisig *self = SHEET_CREATE_MULTISIG(user_data);
  if (!self) return;

  const gchar *npub = g_object_get_data(G_OBJECT(check), "npub");
  gboolean active = gtk_check_button_get_active(check);

  if (active) {
    /* Add to selected list if not already there */
    gboolean found = FALSE;
    for (guint i = 0; i < self->selected_local->len; i++) {
      if (g_strcmp0(npub, g_ptr_array_index(self->selected_local, i)) == 0) {
        found = TRUE;
        break;
      }
    }
    if (!found) {
      g_ptr_array_add(self->selected_local, g_strdup(npub));
    }
  } else {
    /* Remove from selected list */
    for (guint i = 0; i < self->selected_local->len; i++) {
      if (g_strcmp0(npub, g_ptr_array_index(self->selected_local, i)) == 0) {
        g_ptr_array_remove_index(self->selected_local, i);
        break;
      }
    }
  }

  update_local_count(self);
  validate_step2(self);
}

static void update_local_count(SheetCreateMultisig *self) {
  if (!self || !self->lbl_local_count) return;

  guint count = self->selected_local->len;
  guint needed = self->threshold_n;
  guint remote = self->remote_uris->len;

  gchar *text = g_strdup_printf("%u local + %u remote = %u of %u signers",
                                count, remote, count + remote, needed);
  gtk_label_set_text(self->lbl_local_count, text);
  g_free(text);
}

static void validate_step2(SheetCreateMultisig *self) {
  if (!self) return;

  /* Can always proceed - user might want only remote signers */
  if (self->btn_step2_next) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_step2_next), TRUE);
  }
}

static void on_step2_back(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetCreateMultisig *self = SHEET_CREATE_MULTISIG(user_data);
  if (self && self->nav_view) {
    adw_navigation_view_pop(self->nav_view);
  }
}

static void on_step2_next(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetCreateMultisig *self = SHEET_CREATE_MULTISIG(user_data);
  if (!self) return;

  go_to_step(self, STEP_REMOTE);
}

/* ======== Step 3: Remote Signers ======== */

static void update_remote_list(SheetCreateMultisig *self) {
  if (!self || !self->list_remote_signers) return;

  clear_list_box(self->list_remote_signers);

  if (self->remote_uris->len == 0) {
    GtkWidget *label = gtk_label_new("No remote signers added.\n"
                                     "Add bunker URIs above or skip to review.");
    gtk_widget_set_margin_top(label, 20);
    gtk_widget_set_margin_bottom(label, 20);
    gtk_widget_add_css_class(label, "dim-label");
    gtk_list_box_append(self->list_remote_signers, label);
  } else {
    for (guint i = 0; i < self->remote_uris->len; i++) {
      const gchar *uri = g_ptr_array_index(self->remote_uris, i);

      gchar *label = g_strdup_printf("Remote Signer %u", i + 1);
      GtkWidget *row = create_remote_signer_row(uri, label);
      g_free(label);

      /* Connect remove button */
      GtkWidget *btn_remove = g_object_get_data(G_OBJECT(row), "remove_button");
      if (btn_remove) {
        g_object_set_data(G_OBJECT(btn_remove), "sheet", self);
        g_object_set_data(G_OBJECT(btn_remove), "index", GUINT_TO_POINTER(i));
        g_signal_connect(btn_remove, "clicked",
                         G_CALLBACK(on_remove_remote_clicked), self);
      }

      gtk_list_box_append(self->list_remote_signers, row);
    }
  }

  update_remote_count(self);
}

static void update_remote_count(SheetCreateMultisig *self) {
  if (!self || !self->lbl_remote_count) return;

  guint local = self->selected_local->len;
  guint remote = self->remote_uris->len;
  guint needed = self->threshold_n;

  gchar *text = g_strdup_printf("%u local + %u remote = %u of %u signers",
                                local, remote, local + remote, needed);
  gtk_label_set_text(self->lbl_remote_count, text);
  g_free(text);
}

static void on_add_remote_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetCreateMultisig *self = SHEET_CREATE_MULTISIG(user_data);
  if (!self || !self->entry_bunker_uri) return;

  const gchar *uri = gtk_editable_get_text(GTK_EDITABLE(self->entry_bunker_uri));

  /* Validate URI */
  if (!uri || !g_str_has_prefix(uri, "bunker://")) {
    /* Show error - invalid URI */
    return;
  }

  /* Check for duplicates */
  for (guint i = 0; i < self->remote_uris->len; i++) {
    if (g_strcmp0(uri, g_ptr_array_index(self->remote_uris, i)) == 0) {
      /* Already added */
      return;
    }
  }

  /* Check total signer count */
  guint total = self->selected_local->len + self->remote_uris->len;
  if (total >= MAX_SIGNERS) {
    /* Too many signers */
    return;
  }

  g_ptr_array_add(self->remote_uris, g_strdup(uri));

  /* Clear entry */
  gtk_editable_set_text(GTK_EDITABLE(self->entry_bunker_uri), "");

  update_remote_list(self);
  validate_step3(self);
}

static void on_remove_remote_clicked(GtkButton *btn, gpointer user_data) {
  SheetCreateMultisig *self = SHEET_CREATE_MULTISIG(user_data);
  if (!self) return;

  guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "index"));

  if (index < self->remote_uris->len) {
    g_ptr_array_remove_index(self->remote_uris, index);
    update_remote_list(self);
    validate_step3(self);
  }
}

static void validate_step3(SheetCreateMultisig *self) {
  if (!self) return;

  guint total = self->selected_local->len + self->remote_uris->len;
  gboolean valid = total >= self->threshold_n;

  if (self->btn_step3_next) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_step3_next), valid);
  }
}

static void on_step3_back(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetCreateMultisig *self = SHEET_CREATE_MULTISIG(user_data);
  if (self && self->nav_view) {
    adw_navigation_view_pop(self->nav_view);
  }
}

static void on_step3_next(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetCreateMultisig *self = SHEET_CREATE_MULTISIG(user_data);
  if (!self) return;

  populate_review(self);
  go_to_step(self, STEP_REVIEW);
}

/* ======== Step 4: Review ======== */

static void populate_review(SheetCreateMultisig *self) {
  if (!self) return;

  /* Update summary labels */
  if (self->lbl_review_name) {
    gtk_label_set_text(self->lbl_review_name, self->wallet_name);
  }

  if (self->lbl_review_threshold) {
    gchar *threshold = g_strdup_printf("%u of %u signatures required",
                                       self->threshold_m, self->threshold_n);
    gtk_label_set_text(self->lbl_review_threshold, threshold);
    g_free(threshold);
  }

  if (self->lbl_review_signers) {
    guint total = self->selected_local->len + self->remote_uris->len;
    gchar *signers = g_strdup_printf("%u signers (%u local, %u remote)",
                                     total,
                                     self->selected_local->len,
                                     self->remote_uris->len);
    gtk_label_set_text(self->lbl_review_signers, signers);
    g_free(signers);
  }

  /* Populate signer list */
  if (self->list_review_signers) {
    clear_list_box(self->list_review_signers);

    AccountsStore *as = accounts_store_get_default();

    /* Local signers */
    for (guint i = 0; i < self->selected_local->len; i++) {
      const gchar *npub = g_ptr_array_index(self->selected_local, i);
      gchar *name = accounts_store_get_display_name(as, npub);

      AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name ? name : "Local Signer");
      adw_action_row_set_subtitle(row, "Local");

      /* Add local icon */
      GtkWidget *icon = gtk_image_new_from_icon_name("computer-symbolic");
      adw_action_row_add_prefix(row, icon);

      gtk_list_box_append(self->list_review_signers, GTK_WIDGET(row));
      g_free(name);
    }

    /* Remote signers */
    for (guint i = 0; i < self->remote_uris->len; i++) {
      const gchar *uri = g_ptr_array_index(self->remote_uris, i);

      AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());

      gchar *title = g_strdup_printf("Remote Signer %u", i + 1);
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
      g_free(title);

      adw_action_row_set_subtitle(row, "NIP-46 Bunker");

      /* Add remote icon */
      GtkWidget *icon = gtk_image_new_from_icon_name("network-server-symbolic");
      adw_action_row_add_prefix(row, icon);

      gtk_list_box_append(self->list_review_signers, GTK_WIDGET(row));
    }
  }
}

static void on_step4_back(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetCreateMultisig *self = SHEET_CREATE_MULTISIG(user_data);
  if (self && self->nav_view) {
    adw_navigation_view_pop(self->nav_view);
  }
}

static void on_step4_create(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetCreateMultisig *self = SHEET_CREATE_MULTISIG(user_data);
  if (!self) return;

  /* Show spinner */
  if (self->spinner_creating) {
    gtk_spinner_start(self->spinner_creating);
    gtk_widget_set_visible(GTK_WIDGET(self->spinner_creating), TRUE);
  }
  if (self->btn_step4_create) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_step4_create), FALSE);
  }

  /* Create wallet */
  if (create_wallet(self)) {
    go_to_step(self, STEP_SUCCESS);

    if (self->lbl_wallet_id && self->created_wallet_id) {
      gtk_label_set_text(self->lbl_wallet_id, self->created_wallet_id);
    }
  } else {
    /* Show error */
    if (self->spinner_creating) {
      gtk_spinner_stop(self->spinner_creating);
      gtk_widget_set_visible(GTK_WIDGET(self->spinner_creating), FALSE);
    }
    if (self->btn_step4_create) {
      gtk_widget_set_sensitive(GTK_WIDGET(self->btn_step4_create), TRUE);
    }
  }
}

static gboolean create_wallet(SheetCreateMultisig *self) {
  if (!self) return FALSE;

  GError *error = NULL;
  gchar *wallet_id = NULL;

  /* Create the wallet */
  MultisigResult rc = multisig_wallet_create(self->wallet_name,
                                              self->threshold_m,
                                              self->threshold_n,
                                              &wallet_id,
                                              &error);
  if (rc != MULTISIG_OK) {
    g_warning("Failed to create multisig wallet: %s",
              error ? error->message : multisig_result_to_string(rc));
    g_clear_error(&error);
    return FALSE;
  }

  /* Add local co-signers */
  AccountsStore *as = accounts_store_get_default();
  for (guint i = 0; i < self->selected_local->len; i++) {
    const gchar *npub = g_ptr_array_index(self->selected_local, i);
    gchar *label = accounts_store_get_display_name(as, npub);

    MultisigCosigner *cs = multisig_cosigner_new(npub, label, COSIGNER_TYPE_LOCAL);
    cs->is_self = (i == 0);  /* First local signer is self */

    rc = multisig_wallet_add_cosigner(wallet_id, cs, &error);
    if (rc != MULTISIG_OK) {
      g_warning("Failed to add local co-signer: %s",
                error ? error->message : multisig_result_to_string(rc));
      g_clear_error(&error);
    }

    g_free(label);
  }

  /* Add remote co-signers */
  for (guint i = 0; i < self->remote_uris->len; i++) {
    const gchar *uri = g_ptr_array_index(self->remote_uris, i);

    gchar *label = g_strdup_printf("Remote Signer %u", i + 1);
    MultisigCosigner *cs = multisig_cosigner_new_remote(uri, label);
    g_free(label);

    if (cs) {
      rc = multisig_wallet_add_cosigner(wallet_id, cs, &error);
      if (rc != MULTISIG_OK) {
        g_warning("Failed to add remote co-signer: %s",
                  error ? error->message : multisig_result_to_string(rc));
        g_clear_error(&error);
        multisig_cosigner_free(cs);
      }
    }
  }

  self->created_wallet_id = wallet_id;
  g_message("Created multisig wallet: %s", wallet_id);
  return TRUE;
}

/* ======== Step 5: Success ======== */

static void on_finish(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetCreateMultisig *self = SHEET_CREATE_MULTISIG(user_data);
  if (!self) return;

  /* Invoke callback */
  if (self->on_created && self->created_wallet_id) {
    self->on_created(self->created_wallet_id, self->on_created_data);
  }

  /* Close dialog */
  adw_dialog_close(ADW_DIALOG(self));
}

/* ======== Lifecycle ======== */

static void sheet_create_multisig_dispose(GObject *object) {
  SheetCreateMultisig *self = SHEET_CREATE_MULTISIG(object);

  g_free(self->wallet_name);
  g_free(self->created_wallet_id);

  g_clear_pointer(&self->selected_local, g_ptr_array_unref);
  g_clear_pointer(&self->remote_uris, g_ptr_array_unref);

  G_OBJECT_CLASS(sheet_create_multisig_parent_class)->dispose(object);
}

static void sheet_create_multisig_class_init(SheetCreateMultisigClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = sheet_create_multisig_dispose;

  /* Note: In a full implementation, this would load from a .ui resource file */
  /* gtk_widget_class_set_template_from_resource(widget_class,
   *     APP_RESOURCE_PATH "/ui/sheets/sheet-create-multisig.ui"); */
}

static void sheet_create_multisig_init(SheetCreateMultisig *self) {
  self->threshold_m = 2;
  self->threshold_n = 3;
  self->selected_local = g_ptr_array_new_with_free_func(g_free);
  self->remote_uris = g_ptr_array_new_with_free_func(g_free);

  /* Build UI programmatically since we don't have a .ui file yet */
  /* In a full implementation, this would use gtk_widget_init_template() */

  /* Create main container */
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(content, 24);
  gtk_widget_set_margin_end(content, 24);
  gtk_widget_set_margin_top(content, 24);
  gtk_widget_set_margin_bottom(content, 24);

  /* Title */
  GtkWidget *title = gtk_label_new("Create Multi-Signature Wallet");
  gtk_widget_add_css_class(title, "title-1");
  gtk_box_append(GTK_BOX(content), title);

  /* Description */
  GtkWidget *desc = gtk_label_new(
      "Configure a wallet that requires multiple signatures to sign.\n"
      "For example, 2-of-3 means any 2 of 3 co-signers must approve.");
  gtk_label_set_wrap(GTK_LABEL(desc), TRUE);
  gtk_widget_add_css_class(desc, "dim-label");
  gtk_box_append(GTK_BOX(content), desc);

  /* Wallet name */
  AdwPreferencesGroup *group_name = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(group_name, "Wallet Name");

  self->entry_wallet_name = ADW_ENTRY_ROW(adw_entry_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->entry_wallet_name), "Name");
  gtk_editable_set_text(GTK_EDITABLE(self->entry_wallet_name), "My Multisig Wallet");
  adw_preferences_group_add(group_name, GTK_WIDGET(self->entry_wallet_name));
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(group_name));

  g_signal_connect(self->entry_wallet_name, "changed",
                   G_CALLBACK(on_wallet_name_changed), self);

  /* Threshold configuration */
  AdwPreferencesGroup *group_threshold = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(group_threshold, "Signature Threshold");

  /* M spin button */
  GtkWidget *row_m = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *lbl_m = gtk_label_new("Required signatures:");
  gtk_box_append(GTK_BOX(row_m), lbl_m);

  self->spin_threshold_m = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, MAX_SIGNERS, 1));
  gtk_spin_button_set_value(self->spin_threshold_m, 2);
  gtk_box_append(GTK_BOX(row_m), GTK_WIDGET(self->spin_threshold_m));

  adw_preferences_group_add(group_threshold, row_m);

  /* N spin button */
  GtkWidget *row_n = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *lbl_n = gtk_label_new("Total signers:");
  gtk_box_append(GTK_BOX(row_n), lbl_n);

  self->spin_threshold_n = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1, MAX_SIGNERS, 1));
  gtk_spin_button_set_value(self->spin_threshold_n, 3);
  gtk_box_append(GTK_BOX(row_n), GTK_WIDGET(self->spin_threshold_n));

  adw_preferences_group_add(group_threshold, row_n);

  /* Summary */
  self->lbl_threshold_summary = GTK_LABEL(gtk_label_new(""));
  gtk_widget_add_css_class(GTK_WIDGET(self->lbl_threshold_summary), "dim-label");
  adw_preferences_group_add(group_threshold, GTK_WIDGET(self->lbl_threshold_summary));

  gtk_box_append(GTK_BOX(content), GTK_WIDGET(group_threshold));

  g_signal_connect(self->spin_threshold_m, "value-changed",
                   G_CALLBACK(on_threshold_changed), self);
  g_signal_connect(self->spin_threshold_n, "value-changed",
                   G_CALLBACK(on_threshold_changed), self);

  /* Next button */
  self->btn_step1_next = GTK_BUTTON(gtk_button_new_with_label("Continue"));
  gtk_widget_add_css_class(GTK_WIDGET(self->btn_step1_next), "suggested-action");
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(self->btn_step1_next));

  g_signal_connect(self->btn_step1_next, "clicked",
                   G_CALLBACK(on_step1_next), self);

  /* Set as dialog child */
  adw_dialog_set_child(ADW_DIALOG(self), content);
  adw_dialog_set_title(ADW_DIALOG(self), "Create Multisig Wallet");

  /* Initialize */
  update_threshold_summary(self);
  validate_step1(self);
}

SheetCreateMultisig *sheet_create_multisig_new(void) {
  return g_object_new(SHEET_TYPE_CREATE_MULTISIG, NULL);
}

void sheet_create_multisig_set_on_created(SheetCreateMultisig *self,
                                          SheetCreateMultisigCallback callback,
                                          gpointer user_data) {
  g_return_if_fail(SHEET_IS_CREATE_MULTISIG(self));
  self->on_created = callback;
  self->on_created_data = user_data;
}

void sheet_create_multisig_set_default_threshold(SheetCreateMultisig *self,
                                                 guint m,
                                                 guint n) {
  g_return_if_fail(SHEET_IS_CREATE_MULTISIG(self));

  if (n >= m && m >= 1 && n <= MAX_SIGNERS) {
    self->threshold_m = m;
    self->threshold_n = n;

    if (self->spin_threshold_m) {
      gtk_spin_button_set_value(self->spin_threshold_m, (gdouble)m);
    }
    if (self->spin_threshold_n) {
      gtk_spin_button_set_value(self->spin_threshold_n, (gdouble)n);
    }

    update_threshold_summary(self);
  }
}
