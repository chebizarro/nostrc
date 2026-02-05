/* profile-dashboard.c - Profile Dashboard implementation
 *
 * Displays profile header and action button grid for the gnostr-signer app.
 */
#include "profile-dashboard.h"
#include "app-resources.h"
#include "../profile_store.h"
#include "../secret_store.h"

/* Signal IDs */
enum {
  SIGNAL_ACTION_CLICKED,
  N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

/* Properties */
enum {
  PROP_0,
  PROP_NPUB,
  PROP_DISPLAY_NAME,
  PROP_AVATAR_URL,
  N_PROPS
};

static GParamSpec *props[N_PROPS] = { NULL };

struct _ProfileDashboard {
  AdwBin parent_instance;

  /* Profile data */
  gchar *npub;
  gchar *display_name;
  gchar *avatar_url;

  /* Template children - Header */
  GtkLabel *lbl_display_name;
  GtkLabel *lbl_npub;

  /* Template children - Action buttons */
  GtkButton *btn_view_events;
  GtkButton *btn_manage_relays;
  GtkButton *btn_backup_keys;
  GtkButton *btn_change_password;
  GtkButton *btn_sign_message;
};

G_DEFINE_TYPE(ProfileDashboard, profile_dashboard, ADW_TYPE_BIN)

/* Helper: Truncate npub for display (npub1abc...xyz) */
static gchar *truncate_npub(const gchar *npub) {
  if (!npub || strlen(npub) < 20)
    return g_strdup(npub ? npub : "");

  /* Show first 12 chars + ... + last 8 chars */
  gchar *start = g_strndup(npub, 12);
  const gchar *end = npub + strlen(npub) - 8;
  gchar *result = g_strdup_printf("%s...%s", start, end);
  g_free(start);
  return result;
}

/* Update UI from current profile data */
static void update_profile_ui(ProfileDashboard *self) {
  if (!self) return;

  /* Update display name */
  if (self->lbl_display_name) {
    const gchar *name = self->display_name && *self->display_name
                        ? self->display_name
                        : "Anonymous";
    gtk_label_set_text(self->lbl_display_name, name);
  }

  /* Update truncated npub */
  if (self->lbl_npub) {
    gchar *truncated = truncate_npub(self->npub);
    gtk_label_set_text(self->lbl_npub, truncated);
    g_free(truncated);
  }
}

/* Action button clicked handlers - emit signal with action name */
static void on_view_events(GtkButton *btn, gpointer user_data) {
  (void)btn;
  ProfileDashboard *self = PROFILE_DASHBOARD(user_data);
  g_signal_emit(self, signals[SIGNAL_ACTION_CLICKED], 0, "view-events");
}

static void on_manage_relays(GtkButton *btn, gpointer user_data) {
  (void)btn;
  ProfileDashboard *self = PROFILE_DASHBOARD(user_data);
  g_signal_emit(self, signals[SIGNAL_ACTION_CLICKED], 0, "manage-relays");
}

static void on_backup_keys(GtkButton *btn, gpointer user_data) {
  (void)btn;
  ProfileDashboard *self = PROFILE_DASHBOARD(user_data);
  g_signal_emit(self, signals[SIGNAL_ACTION_CLICKED], 0, "backup-keys");
}

static void on_change_password(GtkButton *btn, gpointer user_data) {
  (void)btn;
  ProfileDashboard *self = PROFILE_DASHBOARD(user_data);
  g_signal_emit(self, signals[SIGNAL_ACTION_CLICKED], 0, "change-password");
}

static void on_sign_message(GtkButton *btn, gpointer user_data) {
  (void)btn;
  ProfileDashboard *self = PROFILE_DASHBOARD(user_data);
  g_signal_emit(self, signals[SIGNAL_ACTION_CLICKED], 0, "sign-message");
}

/* GObject property accessors */
static void profile_dashboard_set_property(GObject *object,
                                           guint prop_id,
                                           const GValue *value,
                                           GParamSpec *pspec) {
  ProfileDashboard *self = PROFILE_DASHBOARD(object);

  switch (prop_id) {
    case PROP_NPUB:
      profile_dashboard_set_npub(self, g_value_get_string(value));
      break;
    case PROP_DISPLAY_NAME:
      g_free(self->display_name);
      self->display_name = g_value_dup_string(value);
      update_profile_ui(self);
      g_object_notify_by_pspec(object, props[PROP_DISPLAY_NAME]);
      break;
    case PROP_AVATAR_URL:
      g_free(self->avatar_url);
      self->avatar_url = g_value_dup_string(value);
      update_profile_ui(self);
      g_object_notify_by_pspec(object, props[PROP_AVATAR_URL]);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void profile_dashboard_get_property(GObject *object,
                                           guint prop_id,
                                           GValue *value,
                                           GParamSpec *pspec) {
  ProfileDashboard *self = PROFILE_DASHBOARD(object);

  switch (prop_id) {
    case PROP_NPUB:
      g_value_set_string(value, self->npub);
      break;
    case PROP_DISPLAY_NAME:
      g_value_set_string(value, self->display_name);
      break;
    case PROP_AVATAR_URL:
      g_value_set_string(value, self->avatar_url);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void profile_dashboard_dispose(GObject *object) {
  ProfileDashboard *self = PROFILE_DASHBOARD(object);

  g_clear_pointer(&self->npub, g_free);
  g_clear_pointer(&self->display_name, g_free);
  g_clear_pointer(&self->avatar_url, g_free);

  G_OBJECT_CLASS(profile_dashboard_parent_class)->dispose(object);
}

static void profile_dashboard_class_init(ProfileDashboardClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->set_property = profile_dashboard_set_property;
  object_class->get_property = profile_dashboard_get_property;
  object_class->dispose = profile_dashboard_dispose;

  /* Properties */
  props[PROP_NPUB] = g_param_spec_string(
      "npub", "Npub", "Public key in bech32 format",
      NULL, G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);

  props[PROP_DISPLAY_NAME] = g_param_spec_string(
      "display-name", "Display Name", "Profile display name",
      NULL, G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);

  props[PROP_AVATAR_URL] = g_param_spec_string(
      "avatar-url", "Avatar URL", "Profile avatar image URL",
      NULL, G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY);

  g_object_class_install_properties(object_class, N_PROPS, props);

  /* Signals */
  signals[SIGNAL_ACTION_CLICKED] = g_signal_new(
      "action-clicked",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0, NULL, NULL, NULL,
      G_TYPE_NONE, 1, G_TYPE_STRING);

  /* Template binding */
  gtk_widget_class_set_template_from_resource(
      widget_class, APP_RESOURCE_PATH "/ui/profile-dashboard.ui");

  /* Bind template children - Header */
  gtk_widget_class_bind_template_child(widget_class, ProfileDashboard, lbl_display_name);
  gtk_widget_class_bind_template_child(widget_class, ProfileDashboard, lbl_npub);

  /* Bind template children - Action buttons */
  gtk_widget_class_bind_template_child(widget_class, ProfileDashboard, btn_view_events);
  gtk_widget_class_bind_template_child(widget_class, ProfileDashboard, btn_manage_relays);
  gtk_widget_class_bind_template_child(widget_class, ProfileDashboard, btn_backup_keys);
  gtk_widget_class_bind_template_child(widget_class, ProfileDashboard, btn_change_password);
  gtk_widget_class_bind_template_child(widget_class, ProfileDashboard, btn_sign_message);
}

static void profile_dashboard_init(ProfileDashboard *self) {
  self->npub = NULL;
  self->display_name = NULL;
  self->avatar_url = NULL;

  gtk_widget_init_template(GTK_WIDGET(self));

  /* Connect action button signals */
  if (self->btn_view_events)
    g_signal_connect(self->btn_view_events, "clicked",
                     G_CALLBACK(on_view_events), self);
  if (self->btn_manage_relays)
    g_signal_connect(self->btn_manage_relays, "clicked",
                     G_CALLBACK(on_manage_relays), self);
  if (self->btn_backup_keys)
    g_signal_connect(self->btn_backup_keys, "clicked",
                     G_CALLBACK(on_backup_keys), self);
  if (self->btn_change_password)
    g_signal_connect(self->btn_change_password, "clicked",
                     G_CALLBACK(on_change_password), self);
  if (self->btn_sign_message)
    g_signal_connect(self->btn_sign_message, "clicked",
                     G_CALLBACK(on_sign_message), self);

  /* Initial UI update */
  update_profile_ui(self);
}

/* Public API */

ProfileDashboard *profile_dashboard_new(void) {
  return g_object_new(TYPE_PROFILE_DASHBOARD, NULL);
}

void profile_dashboard_set_npub(ProfileDashboard *self, const gchar *npub) {
  g_return_if_fail(PROFILE_IS_DASHBOARD(self));

  if (g_strcmp0(self->npub, npub) == 0)
    return;

  g_free(self->npub);
  self->npub = g_strdup(npub);

  /* Load profile data for this npub */
  profile_dashboard_refresh(self);

  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_NPUB]);
}

const gchar *profile_dashboard_get_npub(ProfileDashboard *self) {
  g_return_val_if_fail(PROFILE_IS_DASHBOARD(self), NULL);
  return self->npub;
}

void profile_dashboard_refresh(ProfileDashboard *self) {
  g_return_if_fail(PROFILE_IS_DASHBOARD(self));

  if (!self->npub || !*self->npub) {
    /* No npub set - show defaults */
    g_free(self->display_name);
    self->display_name = NULL;
    g_free(self->avatar_url);
    self->avatar_url = NULL;
    update_profile_ui(self);
    return;
  }

  /* Load profile from store */
  ProfileStore *ps = profile_store_new();
  NostrProfile *profile = profile_store_get(ps, self->npub);

  if (profile) {
    g_free(self->display_name);
    self->display_name = g_strdup(profile->name);

    g_free(self->avatar_url);
    self->avatar_url = g_strdup(profile->picture);

    nostr_profile_free(profile);
  }

  profile_store_free(ps);
  update_profile_ui(self);
}
