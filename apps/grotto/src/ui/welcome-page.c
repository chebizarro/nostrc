/* welcome-page.c - Welcome screen shown when no profile exists
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "welcome-page.h"
#include "app-resources.h"

struct _WelcomePage {
  AdwBin parent_instance;

  /* Template children */
  GtkButton *btn_create_profile;
  GtkButton *btn_import_profile;
  GtkButton *btn_settings;
};

G_DEFINE_TYPE(WelcomePage, welcome_page, ADW_TYPE_BIN)

/* Signal IDs */
enum {
  SIGNAL_CREATE_PROFILE,
  SIGNAL_IMPORT_PROFILE,
  SIGNAL_OPEN_SETTINGS,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Signal handlers */
static void
on_create_profile_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  WelcomePage *self = WELCOME_PAGE(user_data);
  g_signal_emit(self, signals[SIGNAL_CREATE_PROFILE], 0);
}

static void
on_import_profile_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  WelcomePage *self = WELCOME_PAGE(user_data);
  g_signal_emit(self, signals[SIGNAL_IMPORT_PROFILE], 0);
}

static void
on_settings_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  WelcomePage *self = WELCOME_PAGE(user_data);
  g_signal_emit(self, signals[SIGNAL_OPEN_SETTINGS], 0);
}

static void
welcome_page_dispose(GObject *object)
{
  gtk_widget_dispose_template(GTK_WIDGET(object), TYPE_WELCOME_PAGE);
  G_OBJECT_CLASS(welcome_page_parent_class)->dispose(object);
}

static void
welcome_page_class_init(WelcomePageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = welcome_page_dispose;

  /* Load template from resource */
  gtk_widget_class_set_template_from_resource(widget_class,
      APP_RESOURCE_PATH "/ui/welcome-page.ui");

  /* Bind template children */
  gtk_widget_class_bind_template_child(widget_class, WelcomePage, btn_create_profile);
  gtk_widget_class_bind_template_child(widget_class, WelcomePage, btn_import_profile);
  gtk_widget_class_bind_template_child(widget_class, WelcomePage, btn_settings);

  /**
   * WelcomePage::create-profile:
   * @self: the #WelcomePage instance
   *
   * Emitted when the user clicks the "Create New Profile" button.
   */
  signals[SIGNAL_CREATE_PROFILE] = g_signal_new(
      "create-profile",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0, NULL, NULL,
      NULL,
      G_TYPE_NONE, 0);

  /**
   * WelcomePage::import-profile:
   * @self: the #WelcomePage instance
   *
   * Emitted when the user clicks the "Import Existing Profile" button.
   */
  signals[SIGNAL_IMPORT_PROFILE] = g_signal_new(
      "import-profile",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0, NULL, NULL,
      NULL,
      G_TYPE_NONE, 0);

  /**
   * WelcomePage::open-settings:
   * @self: the #WelcomePage instance
   *
   * Emitted when the user clicks the settings gear button.
   */
  signals[SIGNAL_OPEN_SETTINGS] = g_signal_new(
      "open-settings",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0, NULL, NULL,
      NULL,
      G_TYPE_NONE, 0);
}

static void
welcome_page_init(WelcomePage *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));

  /* Connect signal handlers */
  g_signal_connect(self->btn_create_profile, "clicked",
                   G_CALLBACK(on_create_profile_clicked), self);
  g_signal_connect(self->btn_import_profile, "clicked",
                   G_CALLBACK(on_import_profile_clicked), self);
  g_signal_connect(self->btn_settings, "clicked",
                   G_CALLBACK(on_settings_clicked), self);
}

WelcomePage *
welcome_page_new(void)
{
  return g_object_new(TYPE_WELCOME_PAGE, NULL);
}
