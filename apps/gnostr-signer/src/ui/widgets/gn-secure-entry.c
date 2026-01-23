/* gn-secure-entry.c - Secure password entry widget implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Uses the secure-memory module for proper memory protection.
 */
#include "gn-secure-entry.h"
#include "../app-resources.h"
#include "../../secure-memory.h"

#include <string.h>
#include <gdk/gdk.h>

/* Default auto-clear timeout in seconds */
#define DEFAULT_TIMEOUT_SECONDS 60

/* Maximum password length for security */
#define MAX_PASSWORD_LENGTH 1024

struct _GnSecureEntry {
  GtkWidget parent_instance;

  /* Template children */
  GtkBox *box_main;
  GtkEntry *entry_password;
  GtkButton *btn_toggle_visibility;
  GtkImage *img_visibility;

  /* Indicators */
  GtkBox *box_indicators;
  GtkLevelBar *level_strength;
  GtkLabel *lbl_strength;
  GtkLabel *lbl_caps_warning;
  GtkLabel *lbl_length_indicator;
  GtkLabel *lbl_requirements;

  /* Secure buffer for password storage */
  gchar *secure_buffer;
  gsize buffer_len;
  gsize buffer_capacity;

  /* Configuration */
  guint min_length;
  guint timeout_seconds;
  gchar *placeholder_text;
  gchar *requirements_text;

  /* State */
  gboolean show_password;
  gboolean show_strength_indicator;
  gboolean show_caps_warning;
  gboolean caps_lock_on;

  /* Timeout handling */
  guint timeout_source_id;
  gint64 last_activity_time;

  /* Cached strength calculation */
  GnPasswordStrength cached_strength;
  gboolean strength_dirty;
};

G_DEFINE_TYPE(GnSecureEntry, gn_secure_entry, GTK_TYPE_WIDGET)

/* Signals */
enum {
  SIGNAL_CHANGED,
  SIGNAL_ACTIVATE,
  SIGNAL_CLEARED,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

/* Properties */
enum {
  PROP_0,
  PROP_TEXT,
  PROP_SHOW_PASSWORD,
  PROP_PLACEHOLDER_TEXT,
  PROP_MIN_LENGTH,
  PROP_TIMEOUT,
  PROP_SHOW_STRENGTH_INDICATOR,
  PROP_SHOW_CAPS_WARNING,
  PROP_REQUIREMENTS_TEXT,
  N_PROPS
};

static GParamSpec *properties[N_PROPS] = { NULL };

/* Forward declarations */
static void gn_secure_entry_update_visibility(GnSecureEntry *self);
static void gn_secure_entry_update_strength(GnSecureEntry *self);
static void gn_secure_entry_update_indicators(GnSecureEntry *self);
static void gn_secure_entry_sync_to_entry(GnSecureEntry *self);
static void gn_secure_entry_start_timeout(GnSecureEntry *self);
static void gn_secure_entry_stop_timeout(GnSecureEntry *self);

/* Secure memory allocation helpers - delegate to secure-memory module */
static gchar *
secure_alloc(gsize size)
{
  return (gchar *)gn_secure_alloc(size);
}

static void
secure_free(gchar *ptr, gsize len)
{
  gn_secure_free(ptr, len);
}

static gchar *
secure_strdup(const gchar *str)
{
  return gn_secure_strdup(str);
}

/* Buffer management */
static void
ensure_buffer_capacity(GnSecureEntry *self, gsize needed)
{
  if (needed <= self->buffer_capacity)
    return;

  gsize new_capacity = MAX(needed, self->buffer_capacity * 2);
  new_capacity = MIN(new_capacity, MAX_PASSWORD_LENGTH + 1);

  gchar *new_buffer = secure_alloc(new_capacity);
  if (self->secure_buffer && self->buffer_len > 0) {
    memcpy(new_buffer, self->secure_buffer, self->buffer_len);
  }

  secure_free(self->secure_buffer, self->buffer_capacity);
  self->secure_buffer = new_buffer;
  self->buffer_capacity = new_capacity;
}

static void
set_buffer_text(GnSecureEntry *self, const gchar *text)
{
  gsize len = text ? strlen(text) : 0;

  /* Enforce maximum length */
  if (len > MAX_PASSWORD_LENGTH)
    len = MAX_PASSWORD_LENGTH;

  ensure_buffer_capacity(self, len + 1);

  /* Clear old content first */
  if (self->buffer_len > 0)
    memset(self->secure_buffer, 0, self->buffer_len);

  if (len > 0) {
    memcpy(self->secure_buffer, text, len);
  }
  self->secure_buffer[len] = '\0';
  self->buffer_len = len;
  self->strength_dirty = TRUE;
}

/* Password strength calculation */
static GnPasswordStrength
calculate_strength(const gchar *password)
{
  if (!password || *password == '\0')
    return GN_PASSWORD_STRENGTH_NONE;

  gsize len = strlen(password);
  gboolean has_lower = FALSE;
  gboolean has_upper = FALSE;
  gboolean has_digit = FALSE;
  gboolean has_special = FALSE;
  int score = 0;

  /* Analyze character classes */
  for (gsize i = 0; i < len && password[i]; i++) {
    guchar c = (guchar)password[i];
    if (c >= 'a' && c <= 'z') has_lower = TRUE;
    else if (c >= 'A' && c <= 'Z') has_upper = TRUE;
    else if (c >= '0' && c <= '9') has_digit = TRUE;
    else if (c >= 0x21 && c <= 0x7E) has_special = TRUE;
  }

  /* Score based on length */
  if (len >= 8) score++;
  if (len >= 12) score++;
  if (len >= 16) score++;
  if (len >= 20) score++;
  if (len >= 24) score++;

  /* Score based on character variety */
  if (has_lower) score++;
  if (has_upper) score++;
  if (has_digit) score++;
  if (has_special) score += 2;

  /* Map score to strength level */
  if (score <= 2) return GN_PASSWORD_STRENGTH_WEAK;
  if (score <= 4) return GN_PASSWORD_STRENGTH_FAIR;
  if (score <= 6) return GN_PASSWORD_STRENGTH_GOOD;
  if (score <= 8) return GN_PASSWORD_STRENGTH_STRONG;
  return GN_PASSWORD_STRENGTH_VERY_STRONG;
}

static const gchar *
get_strength_label(GnPasswordStrength strength)
{
  switch (strength) {
    case GN_PASSWORD_STRENGTH_NONE: return "";
    case GN_PASSWORD_STRENGTH_WEAK: return "Weak";
    case GN_PASSWORD_STRENGTH_FAIR: return "Fair";
    case GN_PASSWORD_STRENGTH_GOOD: return "Good";
    case GN_PASSWORD_STRENGTH_STRONG: return "Strong";
    case GN_PASSWORD_STRENGTH_VERY_STRONG: return "Very Strong";
    default: return "";
  }
}

static const gchar *
get_strength_css_class(GnPasswordStrength strength)
{
  switch (strength) {
    case GN_PASSWORD_STRENGTH_NONE:
    case GN_PASSWORD_STRENGTH_WEAK: return "error";
    case GN_PASSWORD_STRENGTH_FAIR: return "warning";
    case GN_PASSWORD_STRENGTH_GOOD: return "accent";
    case GN_PASSWORD_STRENGTH_STRONG:
    case GN_PASSWORD_STRENGTH_VERY_STRONG: return "success";
    default: return "";
  }
}

/* Clipboard blocking */
static void
on_entry_copy_clipboard(GtkEntry *entry, gpointer user_data)
{
  (void)entry;
  (void)user_data;
  /* Block copy - do nothing */
  g_signal_stop_emission_by_name(entry, "copy-clipboard");
}

static void
on_entry_cut_clipboard(GtkEntry *entry, gpointer user_data)
{
  (void)entry;
  (void)user_data;
  /* Block cut - do nothing */
  g_signal_stop_emission_by_name(entry, "cut-clipboard");
}

static void
on_entry_paste_clipboard(GtkEntry *entry, gpointer user_data)
{
  (void)entry;
  (void)user_data;
  /* Block paste - do nothing */
  g_signal_stop_emission_by_name(entry, "paste-clipboard");
}

/* Entry text changed handler */
static void
on_entry_changed(GtkEditable *editable, gpointer user_data)
{
  GnSecureEntry *self = GN_SECURE_ENTRY(user_data);
  const gchar *text = gtk_editable_get_text(editable);

  /* Update our secure buffer */
  set_buffer_text(self, text);

  /* Reset activity timeout */
  self->last_activity_time = g_get_monotonic_time();
  gn_secure_entry_start_timeout(self);

  /* Update indicators */
  gn_secure_entry_update_strength(self);
  gn_secure_entry_update_indicators(self);

  /* Emit changed signal */
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_TEXT]);
}

/* Entry activated (Enter pressed) */
static void
on_entry_activate(GtkEntry *entry, gpointer user_data)
{
  (void)entry;
  GnSecureEntry *self = GN_SECURE_ENTRY(user_data);
  g_signal_emit(self, signals[SIGNAL_ACTIVATE], 0);
}

/* Toggle visibility button clicked */
static void
on_toggle_visibility_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  GnSecureEntry *self = GN_SECURE_ENTRY(user_data);
  gn_secure_entry_set_show_password(self, !self->show_password);
}

/* Caps lock detection via key events */
static gboolean
on_key_pressed(GtkEventControllerKey *controller,
               guint keyval,
               guint keycode,
               GdkModifierType state,
               gpointer user_data)
{
  (void)controller;
  (void)keyval;
  (void)keycode;
  GnSecureEntry *self = GN_SECURE_ENTRY(user_data);

  /* Check caps lock state */
  gboolean caps_on = (state & GDK_LOCK_MASK) != 0;

  if (caps_on != self->caps_lock_on) {
    self->caps_lock_on = caps_on;
    gn_secure_entry_update_indicators(self);
  }

  /* Reset activity timeout */
  self->last_activity_time = g_get_monotonic_time();

  return FALSE; /* Don't block the event */
}

/* Auto-clear timeout callback */
static gboolean
on_timeout_tick(gpointer user_data)
{
  GnSecureEntry *self = GN_SECURE_ENTRY(user_data);

  if (self->timeout_seconds == 0) {
    self->timeout_source_id = 0;
    return G_SOURCE_REMOVE;
  }

  gint64 now = g_get_monotonic_time();
  gint64 elapsed_us = now - self->last_activity_time;
  gint64 timeout_us = (gint64)self->timeout_seconds * G_USEC_PER_SEC;

  if (elapsed_us >= timeout_us) {
    /* Timeout expired - clear the entry */
    gn_secure_entry_clear(self);
    g_signal_emit(self, signals[SIGNAL_CLEARED], 0);
    self->timeout_source_id = 0;
    return G_SOURCE_REMOVE;
  }

  return G_SOURCE_CONTINUE;
}

static void
gn_secure_entry_start_timeout(GnSecureEntry *self)
{
  if (self->timeout_seconds == 0)
    return;

  if (self->timeout_source_id == 0) {
    /* Check every second */
    self->timeout_source_id = g_timeout_add_seconds(1, on_timeout_tick, self);
  }
}

static void
gn_secure_entry_stop_timeout(GnSecureEntry *self)
{
  if (self->timeout_source_id != 0) {
    g_source_remove(self->timeout_source_id);
    self->timeout_source_id = 0;
  }
}

/* Update visibility state */
static void
gn_secure_entry_update_visibility(GnSecureEntry *self)
{
  if (!self->entry_password)
    return;

  gtk_entry_set_visibility(self->entry_password, self->show_password);

  if (self->img_visibility) {
    const gchar *icon = self->show_password
      ? "view-conceal-symbolic"
      : "view-reveal-symbolic";
    gtk_image_set_from_icon_name(self->img_visibility, icon);
  }

  if (self->btn_toggle_visibility) {
    const gchar *tooltip = self->show_password
      ? "Hide password"
      : "Show password";
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->btn_toggle_visibility), tooltip);

    /* Update accessibility label for screen readers */
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_toggle_visibility),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, tooltip,
                                   -1);
  }

  /* Update entry accessibility description based on visibility */
  if (self->entry_password) {
    const gchar *desc = self->show_password
      ? "Password is visible. Characters are shown."
      : "Password is hidden. Characters are masked.";
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->entry_password),
                                   GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, desc,
                                   -1);
  }
}

/* Update strength indicator */
static void
gn_secure_entry_update_strength(GnSecureEntry *self)
{
  if (!self->strength_dirty)
    return;

  self->cached_strength = calculate_strength(self->secure_buffer);
  self->strength_dirty = FALSE;

  if (self->level_strength && self->show_strength_indicator) {
    gdouble value = (gdouble)self->cached_strength;
    gtk_level_bar_set_value(self->level_strength, value);
    gtk_widget_set_visible(GTK_WIDGET(self->level_strength),
                           self->buffer_len > 0);
  }

  if (self->lbl_strength && self->show_strength_indicator) {
    const gchar *label = get_strength_label(self->cached_strength);
    gtk_label_set_text(self->lbl_strength, label);
    gtk_widget_set_visible(GTK_WIDGET(self->lbl_strength),
                           self->buffer_len > 0);

    /* Update CSS class - use modern GTK4 API */
    GtkWidget *strength_widget = GTK_WIDGET(self->lbl_strength);
    gtk_widget_remove_css_class(strength_widget, "error");
    gtk_widget_remove_css_class(strength_widget, "warning");
    gtk_widget_remove_css_class(strength_widget, "accent");
    gtk_widget_remove_css_class(strength_widget, "success");
    gtk_widget_add_css_class(strength_widget, get_strength_css_class(self->cached_strength));

    /* Update accessibility for strength level bar */
    if (self->level_strength && self->buffer_len > 0) {
      g_autofree gchar *strength_desc = g_strdup_printf("Password strength: %s", label);
      gtk_accessible_update_property(GTK_ACCESSIBLE(self->level_strength),
                                     GTK_ACCESSIBLE_PROPERTY_VALUE_TEXT, strength_desc,
                                     -1);
    }
  }
}

/* Update all indicators */
static void
gn_secure_entry_update_indicators(GnSecureEntry *self)
{
  /* Caps lock warning */
  if (self->lbl_caps_warning && self->show_caps_warning) {
    gboolean was_visible = gtk_widget_get_visible(GTK_WIDGET(self->lbl_caps_warning));
    gtk_widget_set_visible(GTK_WIDGET(self->lbl_caps_warning),
                           self->caps_lock_on);

    /* Announce caps lock state change to screen readers via live region */
    if (self->caps_lock_on && !was_visible && self->entry_password) {
      /* Update the entry's accessible state to include caps lock warning */
      gtk_accessible_update_state(GTK_ACCESSIBLE(self->entry_password),
                                  GTK_ACCESSIBLE_STATE_BUSY, FALSE,
                                  -1);
    }
  }

  /* Length indicator */
  if (self->lbl_length_indicator && self->min_length > 0) {
    gboolean meets_min = self->buffer_len >= self->min_length;

    if (self->buffer_len > 0) {
      g_autofree gchar *text = g_strdup_printf("%zu/%u characters",
                                                self->buffer_len,
                                                self->min_length);
      gtk_label_set_text(self->lbl_length_indicator, text);

      GtkWidget *length_widget = GTK_WIDGET(self->lbl_length_indicator);
      gtk_widget_remove_css_class(length_widget, "error");
      gtk_widget_remove_css_class(length_widget, "success");
      gtk_widget_add_css_class(length_widget, meets_min ? "success" : "error");
      gtk_widget_set_visible(GTK_WIDGET(self->lbl_length_indicator), TRUE);
    } else {
      gtk_widget_set_visible(GTK_WIDGET(self->lbl_length_indicator), FALSE);
    }
  }

  /* Requirements text */
  if (self->lbl_requirements) {
    gboolean show = self->requirements_text && *self->requirements_text && self->buffer_len == 0;
    gtk_widget_set_visible(GTK_WIDGET(self->lbl_requirements), show);
  }
}

/* Sync secure buffer to entry widget */
static void
gn_secure_entry_sync_to_entry(GnSecureEntry *self)
{
  if (!self->entry_password)
    return;

  /* Block our changed handler temporarily */
  g_signal_handlers_block_by_func(self->entry_password, on_entry_changed, self);
  gtk_editable_set_text(GTK_EDITABLE(self->entry_password),
                        self->secure_buffer ? self->secure_buffer : "");
  g_signal_handlers_unblock_by_func(self->entry_password, on_entry_changed, self);
}

/* GObject interface */
static void
gn_secure_entry_set_property(GObject *object,
                              guint prop_id,
                              const GValue *value,
                              GParamSpec *pspec)
{
  GnSecureEntry *self = GN_SECURE_ENTRY(object);

  switch (prop_id) {
    case PROP_TEXT:
      gn_secure_entry_set_text(self, g_value_get_string(value));
      break;
    case PROP_SHOW_PASSWORD:
      gn_secure_entry_set_show_password(self, g_value_get_boolean(value));
      break;
    case PROP_PLACEHOLDER_TEXT:
      gn_secure_entry_set_placeholder_text(self, g_value_get_string(value));
      break;
    case PROP_MIN_LENGTH:
      gn_secure_entry_set_min_length(self, g_value_get_uint(value));
      break;
    case PROP_TIMEOUT:
      gn_secure_entry_set_timeout(self, g_value_get_uint(value));
      break;
    case PROP_SHOW_STRENGTH_INDICATOR:
      gn_secure_entry_set_show_strength_indicator(self, g_value_get_boolean(value));
      break;
    case PROP_SHOW_CAPS_WARNING:
      gn_secure_entry_set_show_caps_warning(self, g_value_get_boolean(value));
      break;
    case PROP_REQUIREMENTS_TEXT:
      gn_secure_entry_set_requirements_text(self, g_value_get_string(value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
gn_secure_entry_get_property(GObject *object,
                              guint prop_id,
                              GValue *value,
                              GParamSpec *pspec)
{
  GnSecureEntry *self = GN_SECURE_ENTRY(object);

  switch (prop_id) {
    case PROP_TEXT:
      g_value_take_string(value, gn_secure_entry_get_text(self));
      break;
    case PROP_SHOW_PASSWORD:
      g_value_set_boolean(value, self->show_password);
      break;
    case PROP_PLACEHOLDER_TEXT:
      g_value_set_string(value, self->placeholder_text);
      break;
    case PROP_MIN_LENGTH:
      g_value_set_uint(value, self->min_length);
      break;
    case PROP_TIMEOUT:
      g_value_set_uint(value, self->timeout_seconds);
      break;
    case PROP_SHOW_STRENGTH_INDICATOR:
      g_value_set_boolean(value, self->show_strength_indicator);
      break;
    case PROP_SHOW_CAPS_WARNING:
      g_value_set_boolean(value, self->show_caps_warning);
      break;
    case PROP_REQUIREMENTS_TEXT:
      g_value_set_string(value, self->requirements_text);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void
gn_secure_entry_dispose(GObject *object)
{
  GnSecureEntry *self = GN_SECURE_ENTRY(object);

  /* Stop the timeout */
  gn_secure_entry_stop_timeout(self);

  /* Clear the secure buffer immediately */
  gn_secure_entry_clear(self);

  /* Dispose of the template children */
  gtk_widget_dispose_template(GTK_WIDGET(self), GN_TYPE_SECURE_ENTRY);

  G_OBJECT_CLASS(gn_secure_entry_parent_class)->dispose(object);
}

static void
gn_secure_entry_finalize(GObject *object)
{
  GnSecureEntry *self = GN_SECURE_ENTRY(object);

  /* Final secure cleanup */
  secure_free(self->secure_buffer, self->buffer_capacity);
  self->secure_buffer = NULL;
  self->buffer_len = 0;
  self->buffer_capacity = 0;

  g_free(self->placeholder_text);
  g_free(self->requirements_text);

  G_OBJECT_CLASS(gn_secure_entry_parent_class)->finalize(object);
}

static void
gn_secure_entry_class_init(GnSecureEntryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->set_property = gn_secure_entry_set_property;
  object_class->get_property = gn_secure_entry_get_property;
  object_class->dispose = gn_secure_entry_dispose;
  object_class->finalize = gn_secure_entry_finalize;

  /* Properties */
  properties[PROP_TEXT] =
    g_param_spec_string("text",
                        "Text",
                        "The password text",
                        NULL,
                        G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_SHOW_PASSWORD] =
    g_param_spec_boolean("show-password",
                         "Show Password",
                         "Whether the password is visible",
                         FALSE,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_PLACEHOLDER_TEXT] =
    g_param_spec_string("placeholder-text",
                        "Placeholder Text",
                        "Text shown when entry is empty",
                        NULL,
                        G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_MIN_LENGTH] =
    g_param_spec_uint("min-length",
                      "Minimum Length",
                      "Minimum required password length",
                      0, MAX_PASSWORD_LENGTH, 0,
                      G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_TIMEOUT] =
    g_param_spec_uint("timeout",
                      "Timeout",
                      "Auto-clear timeout in seconds (0 to disable)",
                      0, 3600, DEFAULT_TIMEOUT_SECONDS,
                      G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_SHOW_STRENGTH_INDICATOR] =
    g_param_spec_boolean("show-strength-indicator",
                         "Show Strength Indicator",
                         "Whether to show password strength indicator",
                         TRUE,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_SHOW_CAPS_WARNING] =
    g_param_spec_boolean("show-caps-warning",
                         "Show Caps Warning",
                         "Whether to show caps lock warning",
                         TRUE,
                         G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  properties[PROP_REQUIREMENTS_TEXT] =
    g_param_spec_string("requirements-text",
                        "Requirements Text",
                        "Password requirements description",
                        NULL,
                        G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, N_PROPS, properties);

  /* Signals */
  signals[SIGNAL_CHANGED] =
    g_signal_new("changed",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 0);

  signals[SIGNAL_ACTIVATE] =
    g_signal_new("activate",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 0);

  signals[SIGNAL_CLEARED] =
    g_signal_new("cleared",
                 G_TYPE_FROM_CLASS(klass),
                 G_SIGNAL_RUN_LAST,
                 0,
                 NULL, NULL,
                 NULL,
                 G_TYPE_NONE, 0);

  /* Load template */
  gtk_widget_class_set_template_from_resource(widget_class,
    APP_RESOURCE_PATH "/ui/widgets/gn-secure-entry.ui");

  /* Bind template children */
  gtk_widget_class_bind_template_child(widget_class, GnSecureEntry, box_main);
  gtk_widget_class_bind_template_child(widget_class, GnSecureEntry, entry_password);
  gtk_widget_class_bind_template_child(widget_class, GnSecureEntry, btn_toggle_visibility);
  gtk_widget_class_bind_template_child(widget_class, GnSecureEntry, img_visibility);
  gtk_widget_class_bind_template_child(widget_class, GnSecureEntry, box_indicators);
  gtk_widget_class_bind_template_child(widget_class, GnSecureEntry, level_strength);
  gtk_widget_class_bind_template_child(widget_class, GnSecureEntry, lbl_strength);
  gtk_widget_class_bind_template_child(widget_class, GnSecureEntry, lbl_caps_warning);
  gtk_widget_class_bind_template_child(widget_class, GnSecureEntry, lbl_length_indicator);
  gtk_widget_class_bind_template_child(widget_class, GnSecureEntry, lbl_requirements);

  /* Set layout manager */
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
}

static void
gn_secure_entry_init(GnSecureEntry *self)
{
  /* Initialize defaults */
  self->timeout_seconds = DEFAULT_TIMEOUT_SECONDS;
  self->show_password = FALSE;
  self->show_strength_indicator = TRUE;
  self->show_caps_warning = TRUE;
  self->strength_dirty = TRUE;
  self->cached_strength = GN_PASSWORD_STRENGTH_NONE;
  self->last_activity_time = g_get_monotonic_time();

  /* Allocate initial secure buffer */
  self->buffer_capacity = 64;
  self->secure_buffer = secure_alloc(self->buffer_capacity);
  self->buffer_len = 0;

  /* Initialize template */
  gtk_widget_init_template(GTK_WIDGET(self));

  /* Set up the entry widget */
  if (self->entry_password) {
    /* Set as password entry */
    gtk_entry_set_visibility(self->entry_password, FALSE);
    gtk_entry_set_input_purpose(self->entry_password, GTK_INPUT_PURPOSE_PASSWORD);

    /* Block clipboard operations */
    g_signal_connect(self->entry_password, "copy-clipboard",
                     G_CALLBACK(on_entry_copy_clipboard), self);
    g_signal_connect(self->entry_password, "cut-clipboard",
                     G_CALLBACK(on_entry_cut_clipboard), self);
    g_signal_connect(self->entry_password, "paste-clipboard",
                     G_CALLBACK(on_entry_paste_clipboard), self);

    /* Connect to text changes */
    g_signal_connect(self->entry_password, "changed",
                     G_CALLBACK(on_entry_changed), self);

    /* Connect activate signal */
    g_signal_connect(self->entry_password, "activate",
                     G_CALLBACK(on_entry_activate), self);

    /* Add key controller for caps lock detection */
    GtkEventController *key_controller = gtk_event_controller_key_new();
    g_signal_connect(key_controller, "key-pressed",
                     G_CALLBACK(on_key_pressed), self);
    gtk_widget_add_controller(GTK_WIDGET(self->entry_password), key_controller);
  }

  /* Connect toggle button */
  if (self->btn_toggle_visibility) {
    g_signal_connect(self->btn_toggle_visibility, "clicked",
                     G_CALLBACK(on_toggle_visibility_clicked), self);
  }

  /* Update initial state */
  gn_secure_entry_update_visibility(self);
  gn_secure_entry_update_indicators(self);

  /* Hide indicators initially */
  if (self->level_strength)
    gtk_widget_set_visible(GTK_WIDGET(self->level_strength), FALSE);
  if (self->lbl_strength)
    gtk_widget_set_visible(GTK_WIDGET(self->lbl_strength), FALSE);
  if (self->lbl_caps_warning)
    gtk_widget_set_visible(GTK_WIDGET(self->lbl_caps_warning), FALSE);
  if (self->lbl_length_indicator)
    gtk_widget_set_visible(GTK_WIDGET(self->lbl_length_indicator), FALSE);
  if (self->lbl_requirements)
    gtk_widget_set_visible(GTK_WIDGET(self->lbl_requirements), FALSE);
}

/* Public API */
GtkWidget *
gn_secure_entry_new(void)
{
  return g_object_new(GN_TYPE_SECURE_ENTRY, NULL);
}

gchar *
gn_secure_entry_get_text(GnSecureEntry *self)
{
  g_return_val_if_fail(GN_IS_SECURE_ENTRY(self), NULL);

  if (!self->secure_buffer || self->buffer_len == 0)
    return NULL;

  return secure_strdup(self->secure_buffer);
}

void
gn_secure_entry_free_text(gchar *text)
{
  if (text) {
    gsize len = strlen(text);
    secure_free(text, len + 1);
  }
}

void
gn_secure_entry_set_text(GnSecureEntry *self, const gchar *text)
{
  g_return_if_fail(GN_IS_SECURE_ENTRY(self));

  set_buffer_text(self, text);
  gn_secure_entry_sync_to_entry(self);
  gn_secure_entry_update_strength(self);
  gn_secure_entry_update_indicators(self);

  self->last_activity_time = g_get_monotonic_time();
  if (self->buffer_len > 0)
    gn_secure_entry_start_timeout(self);

  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_TEXT]);
}

void
gn_secure_entry_clear(GnSecureEntry *self)
{
  g_return_if_fail(GN_IS_SECURE_ENTRY(self));

  /* Clear the secure buffer */
  if (self->secure_buffer && self->buffer_capacity > 0) {
    memset(self->secure_buffer, 0, self->buffer_capacity);
  }
  self->buffer_len = 0;
  self->strength_dirty = TRUE;
  self->cached_strength = GN_PASSWORD_STRENGTH_NONE;

  /* Clear the entry widget */
  if (self->entry_password) {
    g_signal_handlers_block_by_func(self->entry_password, on_entry_changed, self);
    gtk_editable_set_text(GTK_EDITABLE(self->entry_password), "");
    g_signal_handlers_unblock_by_func(self->entry_password, on_entry_changed, self);
  }

  /* Stop timeout */
  gn_secure_entry_stop_timeout(self);

  /* Update display */
  gn_secure_entry_update_strength(self);
  gn_secure_entry_update_indicators(self);

  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_TEXT]);
}

void
gn_secure_entry_set_show_password(GnSecureEntry *self, gboolean show)
{
  g_return_if_fail(GN_IS_SECURE_ENTRY(self));

  if (self->show_password == show)
    return;

  self->show_password = show;
  gn_secure_entry_update_visibility(self);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_SHOW_PASSWORD]);
}

gboolean
gn_secure_entry_get_show_password(GnSecureEntry *self)
{
  g_return_val_if_fail(GN_IS_SECURE_ENTRY(self), FALSE);
  return self->show_password;
}

void
gn_secure_entry_set_placeholder_text(GnSecureEntry *self, const gchar *text)
{
  g_return_if_fail(GN_IS_SECURE_ENTRY(self));

  if (g_strcmp0(self->placeholder_text, text) == 0)
    return;

  g_free(self->placeholder_text);
  self->placeholder_text = g_strdup(text);

  if (self->entry_password) {
    gtk_entry_set_placeholder_text(self->entry_password, text);
  }

  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_PLACEHOLDER_TEXT]);
}

const gchar *
gn_secure_entry_get_placeholder_text(GnSecureEntry *self)
{
  g_return_val_if_fail(GN_IS_SECURE_ENTRY(self), NULL);
  return self->placeholder_text;
}

void
gn_secure_entry_set_min_length(GnSecureEntry *self, guint min_length)
{
  g_return_if_fail(GN_IS_SECURE_ENTRY(self));

  if (self->min_length == min_length)
    return;

  self->min_length = MIN(min_length, MAX_PASSWORD_LENGTH);
  gn_secure_entry_update_indicators(self);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_MIN_LENGTH]);
}

guint
gn_secure_entry_get_min_length(GnSecureEntry *self)
{
  g_return_val_if_fail(GN_IS_SECURE_ENTRY(self), 0);
  return self->min_length;
}

void
gn_secure_entry_set_timeout(GnSecureEntry *self, guint timeout_seconds)
{
  g_return_if_fail(GN_IS_SECURE_ENTRY(self));

  if (self->timeout_seconds == timeout_seconds)
    return;

  self->timeout_seconds = timeout_seconds;

  /* Restart timeout if needed */
  gn_secure_entry_stop_timeout(self);
  if (timeout_seconds > 0 && self->buffer_len > 0) {
    self->last_activity_time = g_get_monotonic_time();
    gn_secure_entry_start_timeout(self);
  }

  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_TIMEOUT]);
}

guint
gn_secure_entry_get_timeout(GnSecureEntry *self)
{
  g_return_val_if_fail(GN_IS_SECURE_ENTRY(self), 0);
  return self->timeout_seconds;
}

GnPasswordStrength
gn_secure_entry_get_strength(GnSecureEntry *self)
{
  g_return_val_if_fail(GN_IS_SECURE_ENTRY(self), GN_PASSWORD_STRENGTH_NONE);

  if (self->strength_dirty) {
    self->cached_strength = calculate_strength(self->secure_buffer);
    self->strength_dirty = FALSE;
  }

  return self->cached_strength;
}

const gchar *
gn_secure_entry_get_strength_text(GnSecureEntry *self)
{
  g_return_val_if_fail(GN_IS_SECURE_ENTRY(self), "");
  return get_strength_label(gn_secure_entry_get_strength(self));
}

gboolean
gn_secure_entry_meets_requirements(GnSecureEntry *self)
{
  g_return_val_if_fail(GN_IS_SECURE_ENTRY(self), FALSE);

  /* Check minimum length */
  if (self->min_length > 0 && self->buffer_len < self->min_length)
    return FALSE;

  /* Check that there's actually content */
  if (self->buffer_len == 0)
    return FALSE;

  return TRUE;
}

void
gn_secure_entry_set_show_strength_indicator(GnSecureEntry *self, gboolean show)
{
  g_return_if_fail(GN_IS_SECURE_ENTRY(self));

  if (self->show_strength_indicator == show)
    return;

  self->show_strength_indicator = show;

  if (self->level_strength)
    gtk_widget_set_visible(GTK_WIDGET(self->level_strength), show && self->buffer_len > 0);
  if (self->lbl_strength)
    gtk_widget_set_visible(GTK_WIDGET(self->lbl_strength), show && self->buffer_len > 0);

  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_SHOW_STRENGTH_INDICATOR]);
}

gboolean
gn_secure_entry_get_show_strength_indicator(GnSecureEntry *self)
{
  g_return_val_if_fail(GN_IS_SECURE_ENTRY(self), FALSE);
  return self->show_strength_indicator;
}

void
gn_secure_entry_set_show_caps_warning(GnSecureEntry *self, gboolean show)
{
  g_return_if_fail(GN_IS_SECURE_ENTRY(self));

  if (self->show_caps_warning == show)
    return;

  self->show_caps_warning = show;
  gn_secure_entry_update_indicators(self);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_SHOW_CAPS_WARNING]);
}

gboolean
gn_secure_entry_get_show_caps_warning(GnSecureEntry *self)
{
  g_return_val_if_fail(GN_IS_SECURE_ENTRY(self), FALSE);
  return self->show_caps_warning;
}

void
gn_secure_entry_set_requirements_text(GnSecureEntry *self, const gchar *text)
{
  g_return_if_fail(GN_IS_SECURE_ENTRY(self));

  if (g_strcmp0(self->requirements_text, text) == 0)
    return;

  g_free(self->requirements_text);
  self->requirements_text = g_strdup(text);

  if (self->lbl_requirements) {
    gtk_label_set_text(self->lbl_requirements, text ? text : "");
  }

  gn_secure_entry_update_indicators(self);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_REQUIREMENTS_TEXT]);
}

const gchar *
gn_secure_entry_get_requirements_text(GnSecureEntry *self)
{
  g_return_val_if_fail(GN_IS_SECURE_ENTRY(self), NULL);
  return self->requirements_text;
}

void
gn_secure_entry_reset_timeout(GnSecureEntry *self)
{
  g_return_if_fail(GN_IS_SECURE_ENTRY(self));
  self->last_activity_time = g_get_monotonic_time();
}

gboolean
gn_secure_entry_grab_focus_entry(GnSecureEntry *self)
{
  g_return_val_if_fail(GN_IS_SECURE_ENTRY(self), FALSE);

  if (self->entry_password) {
    return gtk_widget_grab_focus(GTK_WIDGET(self->entry_password));
  }

  return FALSE;
}
