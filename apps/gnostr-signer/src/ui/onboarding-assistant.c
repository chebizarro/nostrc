/**
 * onboarding-assistant.c - Multi-step onboarding wizard implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include "onboarding-assistant.h"
#include "app-resources.h"
#include <gio/gio.h>
#include <string.h>
#include <math.h>

/* GSettings keys */
#define SIGNER_GSETTINGS_ID "org.gnostr.Signer"
#define ONBOARDING_COMPLETED_KEY "onboarding-completed"

/* Onboarding step indices */
typedef enum {
  STEP_WELCOME = 0,
  STEP_SECURITY,
  STEP_CHOOSE_PATH,
  STEP_CREATE_PASSPHRASE,
  STEP_IMPORT_METHOD,
  STEP_BACKUP_REMINDER,
  STEP_READY,
  STEP_COUNT
} OnboardingStep;

/* User's chosen path */
typedef enum {
  PATH_NONE = 0,
  PATH_CREATE,
  PATH_IMPORT
} OnboardingPath;

struct _OnboardingAssistant {
  AdwWindow parent_instance;

  /* Template children */
  AdwCarousel *carousel;
  AdwCarouselIndicatorDots *carousel_dots;
  GtkButton *btn_back;
  GtkButton *btn_next;
  GtkButton *btn_skip;

  /* Step pages */
  GtkWidget *page_welcome;
  GtkWidget *page_security;
  GtkWidget *page_choose_path;
  GtkWidget *page_create_passphrase;
  GtkWidget *page_import_method;
  GtkWidget *page_backup_reminder;
  GtkWidget *page_ready;

  /* Create passphrase widgets */
  GtkPasswordEntry *entry_passphrase;
  GtkPasswordEntry *entry_passphrase_confirm;
  GtkLevelBar *passphrase_strength;
  GtkLabel *passphrase_hint;

  /* Path selection buttons */
  GtkCheckButton *radio_create;
  GtkCheckButton *radio_import;

  /* Import method widgets */
  GtkCheckButton *radio_import_nsec;
  GtkCheckButton *radio_import_seed;
  GtkCheckButton *radio_import_file;

  /* Backup checkbox */
  GtkCheckButton *backup_understood;

  /* State */
  OnboardingPath chosen_path;
  OnboardingStep current_step;

  /* Callback */
  OnboardingAssistantFinishedCb on_finished;
  gpointer on_finished_data;
};

G_DEFINE_TYPE(OnboardingAssistant, onboarding_assistant, ADW_TYPE_WINDOW)

/* Forward declarations */
static void update_navigation_buttons(OnboardingAssistant *self);
static void go_to_step(OnboardingAssistant *self, OnboardingStep step);
static void update_passphrase_strength(OnboardingAssistant *self);

/* Helper to get GSettings if schema is available */
static GSettings *get_signer_settings(void) {
  GSettingsSchemaSource *source = g_settings_schema_source_get_default();
  if (!source) return NULL;
  GSettingsSchema *schema = g_settings_schema_source_lookup(source, SIGNER_GSETTINGS_ID, TRUE);
  if (!schema) {
    g_debug("GSettings schema %s not found", SIGNER_GSETTINGS_ID);
    return NULL;
  }
  g_settings_schema_unref(schema);
  return g_settings_new(SIGNER_GSETTINGS_ID);
}

/* Calculate passphrase strength (0.0-1.0) */
static gdouble calculate_passphrase_strength(const char *passphrase) {
  if (!passphrase || !*passphrase) return 0.0;

  size_t len = strlen(passphrase);
  gdouble score = 0.0;

  /* Length scoring */
  if (len >= 8) score += 0.2;
  if (len >= 12) score += 0.1;
  if (len >= 16) score += 0.1;
  if (len >= 20) score += 0.1;

  /* Character variety */
  gboolean has_lower = FALSE, has_upper = FALSE, has_digit = FALSE, has_special = FALSE;
  for (size_t i = 0; i < len; i++) {
    char c = passphrase[i];
    if (g_ascii_islower(c)) has_lower = TRUE;
    else if (g_ascii_isupper(c)) has_upper = TRUE;
    else if (g_ascii_isdigit(c)) has_digit = TRUE;
    else has_special = TRUE;
  }

  if (has_lower) score += 0.1;
  if (has_upper) score += 0.1;
  if (has_digit) score += 0.1;
  if (has_special) score += 0.2;

  return fmin(score, 1.0);
}

/* Get strength hint text */
static const char *get_strength_hint(gdouble strength) {
  if (strength < 0.2) return "Very weak - use a longer passphrase";
  if (strength < 0.4) return "Weak - add numbers or symbols";
  if (strength < 0.6) return "Fair - consider making it longer";
  if (strength < 0.8) return "Good - getting stronger";
  return "Strong - excellent passphrase!";
}

static void update_passphrase_strength(OnboardingAssistant *self) {
  if (!self->entry_passphrase || !self->passphrase_strength || !self->passphrase_hint)
    return;

  const char *passphrase = gtk_editable_get_text(GTK_EDITABLE(self->entry_passphrase));
  gdouble strength = calculate_passphrase_strength(passphrase);

  gtk_level_bar_set_value(self->passphrase_strength, strength);
  gtk_label_set_text(self->passphrase_hint, get_strength_hint(strength));

  /* Update level bar colors via CSS classes */
  GtkWidget *bar = GTK_WIDGET(self->passphrase_strength);
  gtk_widget_remove_css_class(bar, "strength-weak");
  gtk_widget_remove_css_class(bar, "strength-fair");
  gtk_widget_remove_css_class(bar, "strength-good");
  gtk_widget_remove_css_class(bar, "strength-strong");

  if (strength < 0.4) {
    gtk_widget_add_css_class(bar, "strength-weak");
  } else if (strength < 0.6) {
    gtk_widget_add_css_class(bar, "strength-fair");
  } else if (strength < 0.8) {
    gtk_widget_add_css_class(bar, "strength-good");
  } else {
    gtk_widget_add_css_class(bar, "strength-strong");
  }
}

/* Get the step index to navigate to based on step enum */
static int get_carousel_position_for_step(OnboardingAssistant *self, OnboardingStep step) {
  /* Steps 0-2 are always shown */
  if (step <= STEP_CHOOSE_PATH) return (int)step;

  /* Steps 3-4 depend on chosen path */
  if (step == STEP_CREATE_PASSPHRASE || step == STEP_IMPORT_METHOD) {
    return 3; /* Both are at position 3, but we swap which page is visible */
  }

  /* Steps 5-6 are backup and ready */
  if (step == STEP_BACKUP_REMINDER) return 4;
  if (step == STEP_READY) return 5;

  return 0;
}

static OnboardingStep get_next_step(OnboardingAssistant *self) {
  switch (self->current_step) {
    case STEP_WELCOME:
      return STEP_SECURITY;
    case STEP_SECURITY:
      return STEP_CHOOSE_PATH;
    case STEP_CHOOSE_PATH:
      return (self->chosen_path == PATH_CREATE) ? STEP_CREATE_PASSPHRASE : STEP_IMPORT_METHOD;
    case STEP_CREATE_PASSPHRASE:
    case STEP_IMPORT_METHOD:
      return STEP_BACKUP_REMINDER;
    case STEP_BACKUP_REMINDER:
      return STEP_READY;
    case STEP_READY:
    default:
      return STEP_READY;
  }
}

static OnboardingStep get_prev_step(OnboardingAssistant *self) {
  switch (self->current_step) {
    case STEP_WELCOME:
      return STEP_WELCOME;
    case STEP_SECURITY:
      return STEP_WELCOME;
    case STEP_CHOOSE_PATH:
      return STEP_SECURITY;
    case STEP_CREATE_PASSPHRASE:
    case STEP_IMPORT_METHOD:
      return STEP_CHOOSE_PATH;
    case STEP_BACKUP_REMINDER:
      return (self->chosen_path == PATH_CREATE) ? STEP_CREATE_PASSPHRASE : STEP_IMPORT_METHOD;
    case STEP_READY:
      return STEP_BACKUP_REMINDER;
    default:
      return STEP_WELCOME;
  }
}

static gboolean can_proceed_from_step(OnboardingAssistant *self) {
  switch (self->current_step) {
    case STEP_CHOOSE_PATH:
      return self->chosen_path != PATH_NONE;

    case STEP_CREATE_PASSPHRASE: {
      if (!self->entry_passphrase || !self->entry_passphrase_confirm)
        return FALSE;
      const char *pass1 = gtk_editable_get_text(GTK_EDITABLE(self->entry_passphrase));
      const char *pass2 = gtk_editable_get_text(GTK_EDITABLE(self->entry_passphrase_confirm));
      if (!pass1 || !*pass1) return FALSE;
      if (g_strcmp0(pass1, pass2) != 0) return FALSE;
      /* Require at least fair strength */
      gdouble strength = calculate_passphrase_strength(pass1);
      return strength >= 0.4;
    }

    case STEP_BACKUP_REMINDER:
      /* Must acknowledge backup importance */
      return self->backup_understood &&
             gtk_check_button_get_active(self->backup_understood);

    default:
      return TRUE;
  }
}

static void update_navigation_buttons(OnboardingAssistant *self) {
  /* Back button: hidden on first step */
  gtk_widget_set_visible(GTK_WIDGET(self->btn_back),
                         self->current_step > STEP_WELCOME);

  /* Next button text changes on last step */
  if (self->current_step == STEP_READY) {
    gtk_button_set_label(self->btn_next, "Get Started");
    gtk_widget_add_css_class(GTK_WIDGET(self->btn_next), "suggested-action");
  } else {
    gtk_button_set_label(self->btn_next, "Next");
    gtk_widget_remove_css_class(GTK_WIDGET(self->btn_next), "suggested-action");
  }

  /* Enable/disable next based on validation */
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_next),
                           can_proceed_from_step(self));

  /* Skip button: always visible except on ready page */
  gtk_widget_set_visible(GTK_WIDGET(self->btn_skip),
                         self->current_step != STEP_READY);
}

static void go_to_step(OnboardingAssistant *self, OnboardingStep step) {
  self->current_step = step;

  /* Calculate actual carousel position */
  int target_pos = get_carousel_position_for_step(self, step);

  /* Get the appropriate page widget */
  GtkWidget *target_page = NULL;
  switch (step) {
    case STEP_WELCOME: target_page = self->page_welcome; break;
    case STEP_SECURITY: target_page = self->page_security; break;
    case STEP_CHOOSE_PATH: target_page = self->page_choose_path; break;
    case STEP_CREATE_PASSPHRASE: target_page = self->page_create_passphrase; break;
    case STEP_IMPORT_METHOD: target_page = self->page_import_method; break;
    case STEP_BACKUP_REMINDER: target_page = self->page_backup_reminder; break;
    case STEP_READY: target_page = self->page_ready; break;
    default: target_page = self->page_welcome; break;
  }

  if (target_page && self->carousel) {
    adw_carousel_scroll_to(self->carousel, target_page, TRUE);
  }

  update_navigation_buttons(self);
}

/* Signal handlers */
static void on_btn_back_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(user_data);
  OnboardingStep prev = get_prev_step(self);
  go_to_step(self, prev);
}

static void on_btn_next_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(user_data);

  if (self->current_step == STEP_READY) {
    /* Onboarding complete! */
    onboarding_assistant_mark_completed();
    if (self->on_finished) {
      self->on_finished(TRUE, self->on_finished_data);
    }
    gtk_window_close(GTK_WINDOW(self));
    return;
  }

  OnboardingStep next = get_next_step(self);
  go_to_step(self, next);
}

/* Callback for skip confirmation dialog */
static void on_skip_dialog_response(GObject *src, GAsyncResult *res, gpointer data) {
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(data);
  GError *err = NULL;
  int response = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, &err);
  if (err) {
    g_clear_error(&err);
    return;
  }

  if (response == 0) {
    /* User chose to skip */
    onboarding_assistant_mark_completed();
    if (self->on_finished) {
      self->on_finished(FALSE, self->on_finished_data);
    }
    gtk_window_close(GTK_WINDOW(self));
  }
}

static void on_btn_skip_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(user_data);

  /* Show warning dialog before skipping */
  GtkAlertDialog *dlg = gtk_alert_dialog_new(
    "Skip Onboarding?\n\n"
    "You can always access onboarding later from Settings.\n"
    "However, we recommend completing it to understand "
    "how gnostr-signer protects your keys.");
  gtk_alert_dialog_set_buttons(dlg, (const char * const[]){
    "Skip Anyway", "Continue Setup", NULL
  });
  gtk_alert_dialog_set_default_button(dlg, 1);
  gtk_alert_dialog_set_cancel_button(dlg, 1);

  gtk_alert_dialog_choose(dlg, GTK_WINDOW(self), NULL, on_skip_dialog_response, self);
  g_object_unref(dlg);
}

static void on_path_toggled(GtkCheckButton *btn, gpointer user_data) {
  (void)btn;
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(user_data);

  if (gtk_check_button_get_active(self->radio_create)) {
    self->chosen_path = PATH_CREATE;
  } else if (gtk_check_button_get_active(self->radio_import)) {
    self->chosen_path = PATH_IMPORT;
  } else {
    self->chosen_path = PATH_NONE;
  }

  update_navigation_buttons(self);
}

static void on_passphrase_changed(GtkEditable *editable, gpointer user_data) {
  (void)editable;
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(user_data);
  update_passphrase_strength(self);
  update_navigation_buttons(self);
}

static void on_backup_toggled(GtkCheckButton *btn, gpointer user_data) {
  (void)btn;
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(user_data);
  update_navigation_buttons(self);
}

static void on_carousel_page_changed(AdwCarousel *carousel, guint index, gpointer user_data) {
  (void)carousel;
  (void)index;
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(user_data);
  update_navigation_buttons(self);
}

static void onboarding_assistant_dispose(GObject *object) {
  OnboardingAssistant *self = ONBOARDING_ASSISTANT(object);
  (void)self;
  G_OBJECT_CLASS(onboarding_assistant_parent_class)->dispose(object);
}

static void onboarding_assistant_class_init(OnboardingAssistantClass *klass) {
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = onboarding_assistant_dispose;

  gtk_widget_class_set_template_from_resource(widget_class,
    APP_RESOURCE_PATH "/ui/onboarding-assistant.ui");

  /* Bind template children */
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, carousel);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, carousel_dots);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, btn_back);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, btn_next);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, btn_skip);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, page_welcome);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, page_security);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, page_choose_path);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, page_create_passphrase);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, page_import_method);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, page_backup_reminder);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, page_ready);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, entry_passphrase);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, entry_passphrase_confirm);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, passphrase_strength);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, passphrase_hint);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, radio_create);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, radio_import);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, radio_import_nsec);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, radio_import_seed);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, radio_import_file);
  gtk_widget_class_bind_template_child(widget_class, OnboardingAssistant, backup_understood);
}

static void onboarding_assistant_init(OnboardingAssistant *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  self->chosen_path = PATH_NONE;
  self->current_step = STEP_WELCOME;
  self->on_finished = NULL;
  self->on_finished_data = NULL;

  /* Connect signals */
  if (self->btn_back)
    g_signal_connect(self->btn_back, "clicked", G_CALLBACK(on_btn_back_clicked), self);
  if (self->btn_next)
    g_signal_connect(self->btn_next, "clicked", G_CALLBACK(on_btn_next_clicked), self);
  if (self->btn_skip)
    g_signal_connect(self->btn_skip, "clicked", G_CALLBACK(on_btn_skip_clicked), self);

  /* Path selection */
  if (self->radio_create)
    g_signal_connect(self->radio_create, "toggled", G_CALLBACK(on_path_toggled), self);
  if (self->radio_import)
    g_signal_connect(self->radio_import, "toggled", G_CALLBACK(on_path_toggled), self);

  /* Passphrase fields */
  if (self->entry_passphrase)
    g_signal_connect(self->entry_passphrase, "changed", G_CALLBACK(on_passphrase_changed), self);
  if (self->entry_passphrase_confirm)
    g_signal_connect(self->entry_passphrase_confirm, "changed", G_CALLBACK(on_passphrase_changed), self);

  /* Backup checkbox */
  if (self->backup_understood)
    g_signal_connect(self->backup_understood, "toggled", G_CALLBACK(on_backup_toggled), self);

  /* Carousel page change */
  if (self->carousel)
    g_signal_connect(self->carousel, "page-changed", G_CALLBACK(on_carousel_page_changed), self);

  /* Initial state */
  update_navigation_buttons(self);
}

OnboardingAssistant *onboarding_assistant_new(void) {
  return g_object_new(TYPE_ONBOARDING_ASSISTANT, NULL);
}

void onboarding_assistant_set_on_finished(OnboardingAssistant *self,
                                          OnboardingAssistantFinishedCb cb,
                                          gpointer user_data) {
  g_return_if_fail(ONBOARDING_IS_ASSISTANT(self));
  self->on_finished = cb;
  self->on_finished_data = user_data;
}

gboolean onboarding_assistant_check_should_show(void) {
  g_autoptr(GSettings) settings = get_signer_settings();
  if (!settings) {
    /* No settings available, assume first run */
    return TRUE;
  }

  return !g_settings_get_boolean(settings, ONBOARDING_COMPLETED_KEY);
}

void onboarding_assistant_mark_completed(void) {
  g_autoptr(GSettings) settings = get_signer_settings();
  if (!settings) return;

  g_settings_set_boolean(settings, ONBOARDING_COMPLETED_KEY, TRUE);
  g_debug("Onboarding marked as completed");
}

void onboarding_assistant_reset(void) {
  g_autoptr(GSettings) settings = get_signer_settings();
  if (!settings) return;

  g_settings_set_boolean(settings, ONBOARDING_COMPLETED_KEY, FALSE);
  g_debug("Onboarding reset - will show on next launch");
}
