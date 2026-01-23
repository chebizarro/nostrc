/* sheet-create-account.c - Account creation wizard implementation
 *
 * Multi-step wizard for creating a new Nostr identity:
 * - Step 1: Enter display name (optional)
 * - Step 2: Create password with strength indicator
 * - Step 3: Show generated BIP-39 seed phrase
 * - Step 4: Verify seed phrase (user enters random words)
 * - Step 5: Success - show npub with copy option
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sheet-create-account.h"
#include "../app-resources.h"
#include "../widgets/gn-secure-entry.h"
#include "../../accounts_store.h"
#include "../../secret_store.h"
#include "../../backup-recovery.h"
#include "../../secure-delete.h"

#include <gtk/gtk.h>
#include <adwaita.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* Step identifiers */
#define STEP_DISPLAY_NAME "step-display-name"
#define STEP_PASSWORD     "step-password"
#define STEP_SEED_PHRASE  "step-seed-phrase"
#define STEP_VERIFY       "step-verify"
#define STEP_SUCCESS      "step-success"

/* Default values */
#define DEFAULT_WORD_COUNT 12
#define VERIFY_WORD_COUNT  3
#define CLIPBOARD_CLEAR_TIMEOUT_SECONDS 60

struct _SheetCreateAccount {
  AdwDialog parent_instance;

  /* Navigation */
  AdwNavigationView *nav_view;
  AdwNavigationPage *page_display_name;
  AdwNavigationPage *page_password;
  AdwNavigationPage *page_seed_phrase;
  AdwNavigationPage *page_verify;
  AdwNavigationPage *page_success;

  /* Step 1: Display Name widgets */
  AdwEntryRow *entry_display_name;
  GtkButton *btn_step1_next;

  /* Step 2: Password widgets */
  GtkBox *box_password_container;
  GtkBox *box_confirm_container;
  GnSecureEntry *secure_password;
  GnSecureEntry *secure_password_confirm;
  GtkLevelBar *password_strength;
  GtkLabel *lbl_password_hint;
  GtkLabel *lbl_password_match;
  GtkButton *btn_step2_back;
  GtkButton *btn_step2_next;

  /* Step 3: Seed Phrase widgets */
  GtkLabel *lbl_seed_phrase;
  GtkButton *btn_copy_seed;
  AdwBanner *banner_seed_warning;
  GtkCheckButton *chk_seed_saved;
  GtkButton *btn_step3_back;
  GtkButton *btn_step3_next;

  /* Step 4: Verify widgets */
  GtkLabel *lbl_verify_instruction;
  AdwEntryRow *entry_verify_word1;
  AdwEntryRow *entry_verify_word2;
  AdwEntryRow *entry_verify_word3;
  GtkLabel *lbl_word1_position;
  GtkLabel *lbl_word2_position;
  GtkLabel *lbl_word3_position;
  AdwBanner *banner_verify_error;
  GtkButton *btn_step4_back;
  GtkButton *btn_step4_next;

  /* Step 5: Success widgets */
  AdwAvatar *avatar_success;
  GtkLabel *lbl_display_name_result;
  GtkLabel *lbl_npub;
  GtkButton *btn_copy_npub;
  GtkButton *btn_finish;

  /* Status */
  GtkBox *box_status;
  GtkSpinner *spinner_status;
  GtkLabel *lbl_status;

  /* State */
  gint word_count;
  gchar *display_name;
  gchar *mnemonic;
  gchar *nsec;
  gchar *npub;
  gint verify_indices[VERIFY_WORD_COUNT];
  gchar **mnemonic_words;
  gint mnemonic_word_count;

  /* Callback */
  SheetCreateAccountCallback on_created;
  gpointer on_created_data;
};

G_DEFINE_TYPE(SheetCreateAccount, sheet_create_account, ADW_TYPE_DIALOG)

/* Forward declarations */
static void go_to_step(SheetCreateAccount *self, const gchar *step);
static void update_password_strength(SheetCreateAccount *self);
static void validate_password_step(SheetCreateAccount *self);
static void generate_seed_phrase(SheetCreateAccount *self);
static void select_verification_words(SheetCreateAccount *self);
static gboolean verify_seed_phrase(SheetCreateAccount *self);
static gboolean create_account(SheetCreateAccount *self);
static void clear_sensitive_data(SheetCreateAccount *self);

/* Securely clear a string */
static void secure_free_string(gchar **str) {
  if (str && *str) {
    gn_secure_shred_string(*str);
    g_free(*str);
    *str = NULL;
  }
}

/* Set status message with spinner */
static void set_status(SheetCreateAccount *self, const gchar *message, gboolean spinning) {
  if (!self) return;

  if (message && *message) {
    if (self->lbl_status) {
      gtk_label_set_text(self->lbl_status, message);
      /* Announce status change to screen readers via live region */
      gtk_accessible_update_property(GTK_ACCESSIBLE(self->lbl_status),
                                     GTK_ACCESSIBLE_PROPERTY_LABEL, message,
                                     -1);
    }
    if (self->spinner_status) gtk_spinner_set_spinning(self->spinner_status, spinning);
    if (self->box_status) gtk_widget_set_visible(GTK_WIDGET(self->box_status), TRUE);
  } else {
    if (self->box_status) gtk_widget_set_visible(GTK_WIDGET(self->box_status), FALSE);
    if (self->spinner_status) gtk_spinner_set_spinning(self->spinner_status, FALSE);
  }
}

/* Copy text to clipboard with auto-clear */
static void copy_to_clipboard(SheetCreateAccount *self, const gchar *text, gboolean schedule_clear) {
  if (!self || !text) return;

  GtkWidget *w = GTK_WIDGET(self);
  GdkDisplay *dpy = gtk_widget_get_display(w);
  if (dpy) {
    GdkClipboard *cb = gdk_display_get_clipboard(dpy);
    if (cb) {
      gdk_clipboard_set_text(cb, text);
      if (schedule_clear) {
        gn_clipboard_clear_after(cb, CLIPBOARD_CLEAR_TIMEOUT_SECONDS);
      }
    }
  }
}

/* Calculate password strength (0.0-1.0) */
static gdouble calculate_password_strength(const char *password) {
  if (!password || !*password) return 0.0;

  size_t len = strlen(password);
  gdouble score = 0.0;

  /* Length scoring */
  if (len >= 8) score += 0.2;
  if (len >= 12) score += 0.1;
  if (len >= 16) score += 0.1;
  if (len >= 20) score += 0.1;

  /* Character variety */
  gboolean has_lower = FALSE, has_upper = FALSE, has_digit = FALSE, has_special = FALSE;
  for (size_t i = 0; i < len; i++) {
    char c = password[i];
    if (g_ascii_islower(c)) has_lower = TRUE;
    else if (g_ascii_isupper(c)) has_upper = TRUE;
    else if (g_ascii_isdigit(c)) has_digit = TRUE;
    else has_special = TRUE;
  }

  if (has_lower) score += 0.1;
  if (has_upper) score += 0.1;
  if (has_digit) score += 0.1;
  if (has_special) score += 0.2;

  return MIN(score, 1.0);
}

/* Get strength hint text */
static const char *get_strength_hint(gdouble strength) {
  if (strength < 0.2) return "Very weak - use a longer password";
  if (strength < 0.4) return "Weak - add numbers or symbols";
  if (strength < 0.6) return "Fair - consider making it longer";
  if (strength < 0.8) return "Good - getting stronger";
  return "Strong - excellent password!";
}

/* Clear all cached sensitive data */
static void clear_sensitive_data(SheetCreateAccount *self) {
  if (!self) return;

  secure_free_string(&self->display_name);
  secure_free_string(&self->mnemonic);
  secure_free_string(&self->nsec);
  g_clear_pointer(&self->npub, g_free);

  if (self->mnemonic_words) {
    for (int i = 0; i < self->mnemonic_word_count; i++) {
      secure_free_string(&self->mnemonic_words[i]);
    }
    g_free(self->mnemonic_words);
    self->mnemonic_words = NULL;
  }
  self->mnemonic_word_count = 0;

  /* Clear secure entries */
  if (self->secure_password)
    gn_secure_entry_clear(self->secure_password);
  if (self->secure_password_confirm)
    gn_secure_entry_clear(self->secure_password_confirm);
}

/* Navigate to a specific step */
static void go_to_step(SheetCreateAccount *self, const gchar *step) {
  if (!self || !self->nav_view || !step) return;

  AdwNavigationPage *page = NULL;

  if (g_strcmp0(step, STEP_DISPLAY_NAME) == 0) {
    page = self->page_display_name;
  } else if (g_strcmp0(step, STEP_PASSWORD) == 0) {
    page = self->page_password;
  } else if (g_strcmp0(step, STEP_SEED_PHRASE) == 0) {
    page = self->page_seed_phrase;
    generate_seed_phrase(self);
  } else if (g_strcmp0(step, STEP_VERIFY) == 0) {
    page = self->page_verify;
    select_verification_words(self);
  } else if (g_strcmp0(step, STEP_SUCCESS) == 0) {
    page = self->page_success;
  }

  if (page) {
    adw_navigation_view_push(self->nav_view, page);
  }
}

/* Handler: Step 1 - Next button */
static void on_step1_next(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetCreateAccount *self = SHEET_CREATE_ACCOUNT(user_data);
  if (!self) return;

  /* Save display name (optional) */
  const gchar *name = gtk_editable_get_text(GTK_EDITABLE(self->entry_display_name));
  g_free(self->display_name);
  self->display_name = g_strdup(name && *name ? name : NULL);

  go_to_step(self, STEP_PASSWORD);
}

/* Update password strength indicator */
static void update_password_strength(SheetCreateAccount *self) {
  if (!self || !self->secure_password) return;

  gchar *password = gn_secure_entry_get_text(self->secure_password);
  gdouble strength = calculate_password_strength(password);

  if (self->password_strength) {
    gtk_level_bar_set_value(self->password_strength, strength);
  }
  if (self->lbl_password_hint) {
    gtk_label_set_text(self->lbl_password_hint, get_strength_hint(strength));
  }

  gn_secure_entry_free_text(password);
}

/* Validate password step */
static void validate_password_step(SheetCreateAccount *self) {
  if (!self) return;

  gchar *password = gn_secure_entry_get_text(self->secure_password);
  gchar *confirm = gn_secure_entry_get_text(self->secure_password_confirm);

  gboolean has_password = password && strlen(password) >= 8;
  gboolean has_confirm = confirm && *confirm;
  gboolean passwords_match = g_strcmp0(password, confirm) == 0;

  /* Update password match indicator */
  if (self->lbl_password_match) {
    if (has_confirm) {
      GtkWidget *match_widget = GTK_WIDGET(self->lbl_password_match);
      if (passwords_match) {
        gtk_label_set_text(self->lbl_password_match, "Passwords match");
        gtk_widget_remove_css_class(match_widget, "error");
        gtk_widget_add_css_class(match_widget, "success");
      } else {
        gtk_label_set_text(self->lbl_password_match, "Passwords do not match");
        gtk_widget_remove_css_class(match_widget, "success");
        gtk_widget_add_css_class(match_widget, "error");
      }
      gtk_widget_set_visible(match_widget, TRUE);
    } else {
      gtk_widget_set_visible(GTK_WIDGET(self->lbl_password_match), FALSE);
    }
  }

  /* Enable/disable next button */
  gboolean valid = has_password && has_confirm && passwords_match;
  if (self->btn_step2_next) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_step2_next), valid);
  }

  gn_secure_entry_free_text(password);
  gn_secure_entry_free_text(confirm);
}

/* Handler: Password entry changed */
static void on_password_changed(GnSecureEntry *entry, gpointer user_data) {
  (void)entry;
  SheetCreateAccount *self = SHEET_CREATE_ACCOUNT(user_data);
  update_password_strength(self);
  validate_password_step(self);
}

/* Handler: Step 2 - Back button */
static void on_step2_back(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetCreateAccount *self = SHEET_CREATE_ACCOUNT(user_data);
  if (self && self->nav_view) {
    adw_navigation_view_pop(self->nav_view);
  }
}

/* Handler: Step 2 - Next button */
static void on_step2_next(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetCreateAccount *self = SHEET_CREATE_ACCOUNT(user_data);
  if (!self) return;

  go_to_step(self, STEP_SEED_PHRASE);
}

/* Generate seed phrase and derive key */
static void generate_seed_phrase(SheetCreateAccount *self) {
  if (!self) return;

  /* Clear any existing mnemonic data */
  secure_free_string(&self->mnemonic);
  secure_free_string(&self->nsec);
  g_clear_pointer(&self->npub, g_free);

  if (self->mnemonic_words) {
    for (int i = 0; i < self->mnemonic_word_count; i++) {
      secure_free_string(&self->mnemonic_words[i]);
    }
    g_free(self->mnemonic_words);
    self->mnemonic_words = NULL;
  }

  GError *error = NULL;
  gchar *mnemonic = NULL;
  gchar *nsec = NULL;

  /* Generate mnemonic and derive key */
  if (!gn_backup_generate_mnemonic(self->word_count, NULL, &mnemonic, &nsec, &error)) {
    g_warning("Failed to generate mnemonic: %s", error ? error->message : "unknown");
    if (self->lbl_seed_phrase) {
      gtk_label_set_text(self->lbl_seed_phrase,
        "Error generating seed phrase. Please try again.");
    }
    g_clear_error(&error);
    return;
  }

  self->mnemonic = mnemonic;
  self->nsec = nsec;

  /* Get npub for display */
  if (!gn_backup_get_npub(nsec, &self->npub, &error)) {
    g_warning("Failed to get npub: %s", error ? error->message : "unknown");
    g_clear_error(&error);
  }

  /* Display the mnemonic */
  if (self->lbl_seed_phrase) {
    /* Format with line breaks for readability */
    gchar **words = g_strsplit(mnemonic, " ", -1);
    self->mnemonic_word_count = g_strv_length(words);

    /* Store words for verification */
    self->mnemonic_words = g_new0(gchar *, self->mnemonic_word_count + 1);
    for (int i = 0; i < self->mnemonic_word_count; i++) {
      self->mnemonic_words[i] = g_strdup(words[i]);
    }

    /* Format as numbered list */
    GString *formatted = g_string_new("");
    for (int i = 0; words[i]; i++) {
      g_string_append_printf(formatted, "%2d. %s", i + 1, words[i]);
      if ((i + 1) % 4 == 0 && words[i + 1]) {
        g_string_append(formatted, "\n");
      } else if (words[i + 1]) {
        g_string_append(formatted, "   ");
      }
    }

    gtk_label_set_text(self->lbl_seed_phrase, formatted->str);
    g_string_free(formatted, TRUE);
    g_strfreev(words);
  }

  /* Reset checkbox and button state */
  if (self->chk_seed_saved) {
    gtk_check_button_set_active(self->chk_seed_saved, FALSE);
  }
  if (self->btn_step3_next) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_step3_next), FALSE);
  }
}

/* Handler: Copy seed phrase */
static void on_copy_seed(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetCreateAccount *self = SHEET_CREATE_ACCOUNT(user_data);
  if (!self || !self->mnemonic) return;

  copy_to_clipboard(self, self->mnemonic, TRUE);

  /* Show warning banner */
  if (self->banner_seed_warning) {
    adw_banner_set_title(self->banner_seed_warning,
      "Seed phrase copied! Clear it after writing down securely.");
    adw_banner_set_revealed(self->banner_seed_warning, TRUE);
  }
}

/* Handler: Seed saved checkbox */
static void on_seed_saved_toggled(GtkCheckButton *btn, gpointer user_data) {
  SheetCreateAccount *self = SHEET_CREATE_ACCOUNT(user_data);
  if (!self) return;

  gboolean saved = gtk_check_button_get_active(btn);
  if (self->btn_step3_next) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_step3_next), saved);
  }
}

/* Handler: Step 3 - Back button */
static void on_step3_back(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetCreateAccount *self = SHEET_CREATE_ACCOUNT(user_data);
  if (self && self->nav_view) {
    adw_navigation_view_pop(self->nav_view);
  }
}

/* Handler: Step 3 - Next button */
static void on_step3_next(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetCreateAccount *self = SHEET_CREATE_ACCOUNT(user_data);
  if (!self) return;

  go_to_step(self, STEP_VERIFY);
}

/* Select random words for verification */
static void select_verification_words(SheetCreateAccount *self) {
  if (!self || !self->mnemonic_words || self->mnemonic_word_count < VERIFY_WORD_COUNT) {
    return;
  }

  /* Select 3 random unique indices */
  srand((unsigned int)time(NULL));

  for (int i = 0; i < VERIFY_WORD_COUNT; i++) {
    gboolean unique = FALSE;
    while (!unique) {
      self->verify_indices[i] = rand() % self->mnemonic_word_count;
      unique = TRUE;
      for (int j = 0; j < i; j++) {
        if (self->verify_indices[j] == self->verify_indices[i]) {
          unique = FALSE;
          break;
        }
      }
    }
  }

  /* Sort indices for better UX */
  for (int i = 0; i < VERIFY_WORD_COUNT - 1; i++) {
    for (int j = i + 1; j < VERIFY_WORD_COUNT; j++) {
      if (self->verify_indices[i] > self->verify_indices[j]) {
        int tmp = self->verify_indices[i];
        self->verify_indices[i] = self->verify_indices[j];
        self->verify_indices[j] = tmp;
      }
    }
  }

  /* Update labels */
  gchar *lbl1 = g_strdup_printf("Word #%d", self->verify_indices[0] + 1);
  gchar *lbl2 = g_strdup_printf("Word #%d", self->verify_indices[1] + 1);
  gchar *lbl3 = g_strdup_printf("Word #%d", self->verify_indices[2] + 1);

  if (self->lbl_word1_position) gtk_label_set_text(self->lbl_word1_position, lbl1);
  if (self->lbl_word2_position) gtk_label_set_text(self->lbl_word2_position, lbl2);
  if (self->lbl_word3_position) gtk_label_set_text(self->lbl_word3_position, lbl3);

  g_free(lbl1);
  g_free(lbl2);
  g_free(lbl3);

  /* Clear entry fields */
  if (self->entry_verify_word1)
    gtk_editable_set_text(GTK_EDITABLE(self->entry_verify_word1), "");
  if (self->entry_verify_word2)
    gtk_editable_set_text(GTK_EDITABLE(self->entry_verify_word2), "");
  if (self->entry_verify_word3)
    gtk_editable_set_text(GTK_EDITABLE(self->entry_verify_word3), "");

  /* Hide error banner */
  if (self->banner_verify_error) {
    adw_banner_set_revealed(self->banner_verify_error, FALSE);
  }

  /* Disable next button initially */
  if (self->btn_step4_next) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_step4_next), FALSE);
  }
}

/* Handler: Verify entry changed */
static void on_verify_entry_changed(GtkEditable *editable, gpointer user_data) {
  (void)editable;
  SheetCreateAccount *self = SHEET_CREATE_ACCOUNT(user_data);
  if (!self) return;

  /* Check if all fields have content */
  const gchar *w1 = gtk_editable_get_text(GTK_EDITABLE(self->entry_verify_word1));
  const gchar *w2 = gtk_editable_get_text(GTK_EDITABLE(self->entry_verify_word2));
  const gchar *w3 = gtk_editable_get_text(GTK_EDITABLE(self->entry_verify_word3));

  gboolean all_filled = w1 && *w1 && w2 && *w2 && w3 && *w3;

  if (self->btn_step4_next) {
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_step4_next), all_filled);
  }

  /* Hide error when user starts typing again */
  if (self->banner_verify_error) {
    adw_banner_set_revealed(self->banner_verify_error, FALSE);
  }
}

/* Verify seed phrase entries */
static gboolean verify_seed_phrase(SheetCreateAccount *self) {
  if (!self || !self->mnemonic_words) return FALSE;

  const gchar *w1 = gtk_editable_get_text(GTK_EDITABLE(self->entry_verify_word1));
  const gchar *w2 = gtk_editable_get_text(GTK_EDITABLE(self->entry_verify_word2));
  const gchar *w3 = gtk_editable_get_text(GTK_EDITABLE(self->entry_verify_word3));

  /* Trim and lowercase for comparison */
  gchar *t1 = g_strstrip(g_ascii_strdown(w1, -1));
  gchar *t2 = g_strstrip(g_ascii_strdown(w2, -1));
  gchar *t3 = g_strstrip(g_ascii_strdown(w3, -1));

  gchar *e1 = g_ascii_strdown(self->mnemonic_words[self->verify_indices[0]], -1);
  gchar *e2 = g_ascii_strdown(self->mnemonic_words[self->verify_indices[1]], -1);
  gchar *e3 = g_ascii_strdown(self->mnemonic_words[self->verify_indices[2]], -1);

  gboolean match = g_strcmp0(t1, e1) == 0 &&
                   g_strcmp0(t2, e2) == 0 &&
                   g_strcmp0(t3, e3) == 0;

  g_free(t1); g_free(t2); g_free(t3);
  g_free(e1); g_free(e2); g_free(e3);

  return match;
}

/* Handler: Step 4 - Back button */
static void on_step4_back(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetCreateAccount *self = SHEET_CREATE_ACCOUNT(user_data);
  if (self && self->nav_view) {
    adw_navigation_view_pop(self->nav_view);
  }
}

/* Handler: Step 4 - Next button (verify and create) */
static void on_step4_next(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetCreateAccount *self = SHEET_CREATE_ACCOUNT(user_data);
  if (!self) return;

  if (!verify_seed_phrase(self)) {
    if (self->banner_verify_error) {
      adw_banner_set_title(self->banner_verify_error,
        "Words don't match. Please check your seed phrase and try again.");
      adw_banner_set_revealed(self->banner_verify_error, TRUE);
    }
    return;
  }

  /* Create the account */
  if (!create_account(self)) {
    if (self->banner_verify_error) {
      adw_banner_set_title(self->banner_verify_error,
        "Failed to create account. Please try again.");
      adw_banner_set_revealed(self->banner_verify_error, TRUE);
    }
    return;
  }

  /* Move to success step */
  go_to_step(self, STEP_SUCCESS);

  /* Update success page */
  if (self->lbl_display_name_result) {
    const gchar *name = self->display_name ? self->display_name : "Anonymous";
    gtk_label_set_text(self->lbl_display_name_result, name);
  }

  if (self->avatar_success) {
    const gchar *name = self->display_name ? self->display_name : "Anonymous";
    adw_avatar_set_text(self->avatar_success, name);
  }

  if (self->lbl_npub && self->npub) {
    /* Truncate npub for display */
    if (strlen(self->npub) > 40) {
      gchar *truncated = g_strdup_printf("%.*s...%s",
                                          16, self->npub,
                                          self->npub + strlen(self->npub) - 8);
      gtk_label_set_text(self->lbl_npub, truncated);
      g_free(truncated);
    } else {
      gtk_label_set_text(self->lbl_npub, self->npub);
    }
  }
}

/* Create the account in secure storage */
static gboolean create_account(SheetCreateAccount *self) {
  if (!self || !self->nsec) return FALSE;

  /* Store in secret store */
  SecretStoreResult result = secret_store_add(self->nsec,
                                               self->display_name,
                                               TRUE);

  if (result != SECRET_STORE_OK) {
    g_warning("Failed to store key: %s", secret_store_result_to_string(result));
    return FALSE;
  }

  /* Add to accounts store */
  AccountsStore *as = accounts_store_get_default();
  if (as && self->npub) {
    accounts_store_add(as, self->npub, self->display_name);
    accounts_store_set_active(as, self->npub);
    accounts_store_save(as);
  }

  return TRUE;
}

/* Handler: Copy npub */
static void on_copy_npub(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetCreateAccount *self = SHEET_CREATE_ACCOUNT(user_data);
  if (!self || !self->npub) return;

  copy_to_clipboard(self, self->npub, FALSE);
}

/* Handler: Finish button */
static void on_finish(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetCreateAccount *self = SHEET_CREATE_ACCOUNT(user_data);
  if (!self) return;

  /* Invoke callback */
  if (self->on_created && self->npub) {
    self->on_created(self->npub, self->on_created_data);
  }

  /* Clear sensitive data */
  clear_sensitive_data(self);

  /* Close dialog */
  adw_dialog_close(ADW_DIALOG(self));
}

/* Handler: Dialog closed */
static void on_dialog_closed(AdwDialog *dialog, gpointer user_data) {
  (void)dialog;
  SheetCreateAccount *self = SHEET_CREATE_ACCOUNT(user_data);
  clear_sensitive_data(self);
}

static void sheet_create_account_dispose(GObject *object) {
  SheetCreateAccount *self = SHEET_CREATE_ACCOUNT(object);
  clear_sensitive_data(self);
  G_OBJECT_CLASS(sheet_create_account_parent_class)->dispose(object);
}

static void sheet_create_account_class_init(SheetCreateAccountClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = sheet_create_account_dispose;

  gtk_widget_class_set_template_from_resource(widget_class,
    APP_RESOURCE_PATH "/ui/sheets/sheet-create-account.ui");

  /* Navigation */
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, nav_view);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, page_display_name);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, page_password);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, page_seed_phrase);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, page_verify);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, page_success);

  /* Step 1 widgets */
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, entry_display_name);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, btn_step1_next);

  /* Step 2 widgets */
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, box_password_container);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, box_confirm_container);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, password_strength);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, lbl_password_hint);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, lbl_password_match);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, btn_step2_back);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, btn_step2_next);

  /* Step 3 widgets */
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, lbl_seed_phrase);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, btn_copy_seed);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, banner_seed_warning);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, chk_seed_saved);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, btn_step3_back);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, btn_step3_next);

  /* Step 4 widgets */
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, lbl_verify_instruction);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, entry_verify_word1);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, entry_verify_word2);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, entry_verify_word3);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, lbl_word1_position);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, lbl_word2_position);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, lbl_word3_position);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, banner_verify_error);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, btn_step4_back);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, btn_step4_next);

  /* Step 5 widgets */
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, avatar_success);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, lbl_display_name_result);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, lbl_npub);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, btn_copy_npub);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, btn_finish);

  /* Status */
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, box_status);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, spinner_status);
  gtk_widget_class_bind_template_child(widget_class, SheetCreateAccount, lbl_status);
}

static void sheet_create_account_init(SheetCreateAccount *self) {
  /* Ensure GnSecureEntry type is registered */
  g_type_ensure(GN_TYPE_SECURE_ENTRY);

  self->word_count = DEFAULT_WORD_COUNT;

  gtk_widget_init_template(GTK_WIDGET(self));

  /* Create secure password entries */
  self->secure_password = GN_SECURE_ENTRY(gn_secure_entry_new());
  gn_secure_entry_set_placeholder_text(self->secure_password, "Enter password");
  gn_secure_entry_set_min_length(self->secure_password, 8);
  gn_secure_entry_set_show_strength_indicator(self->secure_password, TRUE);
  gn_secure_entry_set_show_caps_warning(self->secure_password, TRUE);
  gn_secure_entry_set_timeout(self->secure_password, 120);

  if (self->box_password_container) {
    gtk_box_append(self->box_password_container, GTK_WIDGET(self->secure_password));
  }

  self->secure_password_confirm = GN_SECURE_ENTRY(gn_secure_entry_new());
  gn_secure_entry_set_placeholder_text(self->secure_password_confirm, "Confirm password");
  gn_secure_entry_set_min_length(self->secure_password_confirm, 8);
  gn_secure_entry_set_show_strength_indicator(self->secure_password_confirm, FALSE);
  gn_secure_entry_set_show_caps_warning(self->secure_password_confirm, TRUE);
  gn_secure_entry_set_timeout(self->secure_password_confirm, 120);

  if (self->box_confirm_container) {
    gtk_box_append(self->box_confirm_container, GTK_WIDGET(self->secure_password_confirm));
  }

  /* Connect signals */
  g_signal_connect(self->secure_password, "changed",
                   G_CALLBACK(on_password_changed), self);
  g_signal_connect(self->secure_password_confirm, "changed",
                   G_CALLBACK(on_password_changed), self);

  /* Step 1 button */
  if (self->btn_step1_next) {
    g_signal_connect(self->btn_step1_next, "clicked",
                     G_CALLBACK(on_step1_next), self);
  }

  /* Step 2 buttons */
  if (self->btn_step2_back) {
    g_signal_connect(self->btn_step2_back, "clicked",
                     G_CALLBACK(on_step2_back), self);
  }
  if (self->btn_step2_next) {
    g_signal_connect(self->btn_step2_next, "clicked",
                     G_CALLBACK(on_step2_next), self);
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_step2_next), FALSE);
  }

  /* Step 3 widgets */
  if (self->btn_copy_seed) {
    g_signal_connect(self->btn_copy_seed, "clicked",
                     G_CALLBACK(on_copy_seed), self);
  }
  if (self->chk_seed_saved) {
    g_signal_connect(self->chk_seed_saved, "toggled",
                     G_CALLBACK(on_seed_saved_toggled), self);
  }
  if (self->btn_step3_back) {
    g_signal_connect(self->btn_step3_back, "clicked",
                     G_CALLBACK(on_step3_back), self);
  }
  if (self->btn_step3_next) {
    g_signal_connect(self->btn_step3_next, "clicked",
                     G_CALLBACK(on_step3_next), self);
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_step3_next), FALSE);
  }

  /* Step 4 widgets */
  if (self->entry_verify_word1) {
    g_signal_connect(self->entry_verify_word1, "changed",
                     G_CALLBACK(on_verify_entry_changed), self);
  }
  if (self->entry_verify_word2) {
    g_signal_connect(self->entry_verify_word2, "changed",
                     G_CALLBACK(on_verify_entry_changed), self);
  }
  if (self->entry_verify_word3) {
    g_signal_connect(self->entry_verify_word3, "changed",
                     G_CALLBACK(on_verify_entry_changed), self);
  }
  if (self->btn_step4_back) {
    g_signal_connect(self->btn_step4_back, "clicked",
                     G_CALLBACK(on_step4_back), self);
  }
  if (self->btn_step4_next) {
    g_signal_connect(self->btn_step4_next, "clicked",
                     G_CALLBACK(on_step4_next), self);
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_step4_next), FALSE);
  }

  /* Step 5 widgets */
  if (self->btn_copy_npub) {
    g_signal_connect(self->btn_copy_npub, "clicked",
                     G_CALLBACK(on_copy_npub), self);
  }
  if (self->btn_finish) {
    g_signal_connect(self->btn_finish, "clicked",
                     G_CALLBACK(on_finish), self);
  }

  /* Dialog closed handler */
  g_signal_connect(self, "closed", G_CALLBACK(on_dialog_closed), self);
}

SheetCreateAccount *sheet_create_account_new(void) {
  return g_object_new(SHEET_TYPE_CREATE_ACCOUNT, NULL);
}

void sheet_create_account_set_on_created(SheetCreateAccount *self,
                                          SheetCreateAccountCallback callback,
                                          gpointer user_data) {
  g_return_if_fail(SHEET_IS_CREATE_ACCOUNT(self));
  self->on_created = callback;
  self->on_created_data = user_data;
}

void sheet_create_account_set_word_count(SheetCreateAccount *self, gint word_count) {
  g_return_if_fail(SHEET_IS_CREATE_ACCOUNT(self));

  /* Only allow valid BIP-39 word counts */
  if (word_count == 12 || word_count == 15 || word_count == 18 ||
      word_count == 21 || word_count == 24) {
    self->word_count = word_count;
  }
}
