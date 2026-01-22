#include "gnostr-profile-edit.h"
#include "../ipc/signer_ipc.h"
#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <jansson.h>
#include <time.h>

#define UI_RESOURCE "/org/gnostr/ui/ui/dialogs/gnostr-profile-edit.ui"

struct _GnostrProfileEdit {
  GtkWindow parent_instance;

  /* Template children */
  GtkWidget *entry_display_name;
  GtkWidget *entry_name;
  GtkWidget *text_about;
  GtkWidget *entry_picture;
  GtkWidget *entry_banner;
  GtkWidget *entry_nip05;
  GtkWidget *entry_website;
  GtkWidget *entry_lud16;
  GtkWidget *btn_cancel;
  GtkWidget *btn_save;
  GtkWidget *spinner;
  GtkWidget *toast_revealer;
  GtkWidget *toast_label;

  /* State */
  gboolean saving;
  json_t *original_json;  /* Preserve unknown fields */
};

G_DEFINE_TYPE(GnostrProfileEdit, gnostr_profile_edit, GTK_TYPE_WINDOW)

/* Signals */
enum {
  SIGNAL_PROFILE_SAVED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

/* Forward declarations */
static void on_cancel_clicked(GtkButton *btn, gpointer user_data);
static void on_save_clicked(GtkButton *btn, gpointer user_data);
static void show_toast(GnostrProfileEdit *self, const char *msg);

static void gnostr_profile_edit_dispose(GObject *obj) {
  GnostrProfileEdit *self = GNOSTR_PROFILE_EDIT(obj);
  if (self->original_json) {
    json_decref(self->original_json);
    self->original_json = NULL;
  }
  gtk_widget_dispose_template(GTK_WIDGET(self), GNOSTR_TYPE_PROFILE_EDIT);
  G_OBJECT_CLASS(gnostr_profile_edit_parent_class)->dispose(obj);
}

static void gnostr_profile_edit_finalize(GObject *obj) {
  G_OBJECT_CLASS(gnostr_profile_edit_parent_class)->finalize(obj);
}

static void gnostr_profile_edit_class_init(GnostrProfileEditClass *klass) {
  GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);
  GObjectClass *gclass = G_OBJECT_CLASS(klass);

  gclass->dispose = gnostr_profile_edit_dispose;
  gclass->finalize = gnostr_profile_edit_finalize;

  gtk_widget_class_set_template_from_resource(wclass, UI_RESOURCE);

  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, entry_display_name);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, entry_name);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, text_about);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, entry_picture);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, entry_banner);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, entry_nip05);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, entry_website);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, entry_lud16);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, btn_cancel);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, btn_save);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, spinner);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, toast_revealer);
  gtk_widget_class_bind_template_child(wclass, GnostrProfileEdit, toast_label);

  gtk_widget_class_bind_template_callback(wclass, on_cancel_clicked);
  gtk_widget_class_bind_template_callback(wclass, on_save_clicked);

  /**
   * GnostrProfileEdit::profile-saved:
   * @self: the profile edit dialog
   * @profile_json: the new profile JSON content
   *
   * Emitted when the profile has been successfully signed and published.
   */
  signals[SIGNAL_PROFILE_SAVED] = g_signal_new(
    "profile-saved",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1,
    G_TYPE_STRING);
}

static void gnostr_profile_edit_init(GnostrProfileEdit *self) {
  gtk_widget_init_template(GTK_WIDGET(self));
  self->saving = FALSE;
  self->original_json = NULL;

  /* Connect button signals */
  g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel_clicked), self);
  g_signal_connect(self->btn_save, "clicked", G_CALLBACK(on_save_clicked), self);
}

GnostrProfileEdit *gnostr_profile_edit_new(GtkWindow *parent) {
  GnostrProfileEdit *self = g_object_new(GNOSTR_TYPE_PROFILE_EDIT,
                                          "transient-for", parent,
                                          "modal", TRUE,
                                          NULL);
  return self;
}

static void show_toast(GnostrProfileEdit *self, const char *msg) {
  if (!self->toast_label || !self->toast_revealer) return;
  gtk_label_set_text(GTK_LABEL(self->toast_label), msg);
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->toast_revealer), TRUE);
  /* Auto-hide after 3 seconds */
  g_timeout_add_seconds(3, (GSourceFunc)gtk_revealer_set_reveal_child,
                        self->toast_revealer);
}

static void set_ui_sensitive(GnostrProfileEdit *self, gboolean sensitive) {
  gtk_widget_set_sensitive(self->entry_display_name, sensitive);
  gtk_widget_set_sensitive(self->entry_name, sensitive);
  gtk_widget_set_sensitive(self->text_about, sensitive);
  gtk_widget_set_sensitive(self->entry_picture, sensitive);
  gtk_widget_set_sensitive(self->entry_banner, sensitive);
  gtk_widget_set_sensitive(self->entry_nip05, sensitive);
  gtk_widget_set_sensitive(self->entry_website, sensitive);
  gtk_widget_set_sensitive(self->entry_lud16, sensitive);
  gtk_widget_set_sensitive(self->btn_save, sensitive);
  gtk_widget_set_sensitive(self->btn_cancel, sensitive);

  if (self->spinner) {
    gtk_widget_set_visible(self->spinner, !sensitive);
    if (!sensitive) {
      gtk_spinner_start(GTK_SPINNER(self->spinner));
    } else {
      gtk_spinner_stop(GTK_SPINNER(self->spinner));
    }
  }
}

void gnostr_profile_edit_set_profile_json(GnostrProfileEdit *self, const char *profile_json) {
  g_return_if_fail(GNOSTR_IS_PROFILE_EDIT(self));

  /* Clear any previous original JSON */
  if (self->original_json) {
    json_decref(self->original_json);
    self->original_json = NULL;
  }

  if (!profile_json || !*profile_json) {
    return;
  }

  json_error_t error;
  json_t *root = json_loads(profile_json, 0, &error);
  if (!root) {
    g_warning("ProfileEdit: failed to parse profile JSON: %s", error.text);
    return;
  }

  /* Store original for preserving unknown fields */
  self->original_json = json_deep_copy(root);

  /* Extract and populate fields */
  json_t *val;

  if ((val = json_object_get(root, "display_name")) && json_is_string(val)) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_display_name), json_string_value(val));
  }
  if ((val = json_object_get(root, "name")) && json_is_string(val)) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_name), json_string_value(val));
  }
  if ((val = json_object_get(root, "about")) && json_is_string(val)) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->text_about));
    gtk_text_buffer_set_text(buffer, json_string_value(val), -1);
  }
  if ((val = json_object_get(root, "picture")) && json_is_string(val)) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_picture), json_string_value(val));
  }
  if ((val = json_object_get(root, "banner")) && json_is_string(val)) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_banner), json_string_value(val));
  }
  if ((val = json_object_get(root, "nip05")) && json_is_string(val)) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_nip05), json_string_value(val));
  }
  if ((val = json_object_get(root, "website")) && json_is_string(val)) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_website), json_string_value(val));
  }
  if ((val = json_object_get(root, "lud16")) && json_is_string(val)) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_lud16), json_string_value(val));
  }

  json_decref(root);
}

char *gnostr_profile_edit_get_profile_json(GnostrProfileEdit *self) {
  g_return_val_if_fail(GNOSTR_IS_PROFILE_EDIT(self), NULL);

  /* Start with original JSON to preserve unknown fields, or create new object */
  json_t *root = self->original_json ? json_deep_copy(self->original_json) : json_object();

  /* Helper macro to set or remove a field */
  #define SET_OR_REMOVE(field_name, widget) do { \
    const char *text = gtk_editable_get_text(GTK_EDITABLE(widget)); \
    if (text && *text) { \
      json_object_set_new(root, field_name, json_string(text)); \
    } else { \
      json_object_del(root, field_name); \
    } \
  } while (0)

  SET_OR_REMOVE("display_name", self->entry_display_name);
  SET_OR_REMOVE("name", self->entry_name);
  SET_OR_REMOVE("picture", self->entry_picture);
  SET_OR_REMOVE("banner", self->entry_banner);
  SET_OR_REMOVE("nip05", self->entry_nip05);
  SET_OR_REMOVE("website", self->entry_website);
  SET_OR_REMOVE("lud16", self->entry_lud16);

  #undef SET_OR_REMOVE

  /* Handle about field (TextView) */
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->text_about));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  char *about_text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  if (about_text && *about_text) {
    json_object_set_new(root, "about", json_string(about_text));
  } else {
    json_object_del(root, "about");
  }
  g_free(about_text);

  char *result = json_dumps(root, JSON_COMPACT);
  json_decref(root);
  return result;
}

static void on_cancel_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrProfileEdit *self = GNOSTR_PROFILE_EDIT(user_data);
  gtk_window_close(GTK_WINDOW(self));
}

/* Context for async sign-and-publish */
typedef struct {
  GnostrProfileEdit *self;
  char *profile_content;
} ProfilePublishContext;

static void profile_publish_context_free(ProfilePublishContext *ctx) {
  if (!ctx) return;
  g_free(ctx->profile_content);
  g_free(ctx);
}

/* Callback when DBus sign_event completes for profile */
static void on_profile_sign_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  ProfilePublishContext *ctx = (ProfilePublishContext *)user_data;
  if (!ctx) return;

  GnostrProfileEdit *self = ctx->self;
  if (!GNOSTR_IS_PROFILE_EDIT(self)) {
    profile_publish_context_free(ctx);
    return;
  }

  NostrSignerProxy *proxy = NOSTR_ORG_NOSTR_SIGNER(source);

  GError *error = NULL;
  char *signed_event_json = NULL;
  gboolean ok = nostr_org_nostr_signer_call_sign_event_finish(proxy, &signed_event_json, res, &error);

  if (!ok || !signed_event_json) {
    char *msg = g_strdup_printf("Signing failed: %s", error ? error->message : "unknown error");
    show_toast(self, msg);
    g_free(msg);
    g_clear_error(&error);
    set_ui_sensitive(self, TRUE);
    self->saving = FALSE;
    profile_publish_context_free(ctx);
    return;
  }

  g_message("[PROFILE_EDIT] Signed event: %.100s...", signed_event_json);

  /* Profile signed successfully - emit signal with the new profile content.
   * The main window / SimplePool will handle actual relay publishing. */
  set_ui_sensitive(self, TRUE);
  self->saving = FALSE;

  show_toast(self, "Profile saved!");

  /* Emit signal with the new profile content */
  g_signal_emit(self, signals[SIGNAL_PROFILE_SAVED], 0, ctx->profile_content);

  /* Close dialog after short delay */
  g_timeout_add(1500, (GSourceFunc)gtk_window_close, self);

  /* Cleanup */
  g_free(signed_event_json);
  profile_publish_context_free(ctx);
}

static void on_save_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrProfileEdit *self = GNOSTR_PROFILE_EDIT(user_data);

  if (self->saving) return;

  /* Get signer proxy */
  GError *proxy_err = NULL;
  NostrSignerProxy *proxy = gnostr_signer_proxy_get(&proxy_err);
  if (!proxy) {
    char *msg = g_strdup_printf("Signer not available: %s", proxy_err ? proxy_err->message : "not connected");
    show_toast(self, msg);
    g_free(msg);
    g_clear_error(&proxy_err);
    return;
  }

  /* Disable UI while saving */
  self->saving = TRUE;
  set_ui_sensitive(self, FALSE);
  show_toast(self, "Signing profile...");

  /* Build profile content JSON */
  char *profile_content = gnostr_profile_edit_get_profile_json(self);
  if (!profile_content) {
    show_toast(self, "Failed to serialize profile");
    set_ui_sensitive(self, TRUE);
    self->saving = FALSE;
    return;
  }

  /* Build unsigned kind 0 event JSON */
  json_t *event_obj = json_object();
  json_object_set_new(event_obj, "kind", json_integer(0));
  json_object_set_new(event_obj, "created_at", json_integer((json_int_t)time(NULL)));
  json_object_set_new(event_obj, "content", json_string(profile_content));
  json_object_set_new(event_obj, "tags", json_array());

  char *event_json = json_dumps(event_obj, JSON_COMPACT);
  json_decref(event_obj);

  if (!event_json) {
    show_toast(self, "Failed to build event JSON");
    g_free(profile_content);
    set_ui_sensitive(self, TRUE);
    self->saving = FALSE;
    return;
  }

  g_message("[PROFILE_EDIT] Unsigned event: %s", event_json);

  /* Create async context */
  ProfilePublishContext *ctx = g_new0(ProfilePublishContext, 1);
  ctx->self = self;
  ctx->profile_content = profile_content; /* Transfer ownership */

  /* Call signer asynchronously */
  nostr_org_nostr_signer_call_sign_event(
    proxy,
    event_json,
    "",        /* current_user: empty = use default */
    "gnostr",  /* app_id */
    NULL,      /* cancellable */
    on_profile_sign_complete,
    ctx
  );
  g_free(event_json);
}
