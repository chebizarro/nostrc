/* sheet-profile-editor.c - Profile editing dialog implementation
 *
 * Provides a UI for editing Nostr profile metadata (kind:0 events).
 * Features:
 * - Edit all standard profile fields (name, about, picture, banner, nip05, lud16, website)
 * - Preview changes before publishing
 * - Sign events using the signer's key management
 * - Publish as kind:0 metadata events
 */
#include "sheet-profile-editor.h"
#include "../app-resources.h"
#include "../../profile_store.h"
#include "../../secret_store.h"
#include "../../relay_store.h"

#include <json-glib/json-glib.h>
#include <time.h>
#include <string.h>

struct _SheetProfileEditor {
  AdwDialog parent_instance;

  /* Template children */
  GtkButton *btn_cancel;
  GtkButton *btn_preview;
  GtkButton *btn_save;
  AdwEntryRow *entry_name;
  AdwEntryRow *entry_about;
  AdwEntryRow *entry_picture;
  AdwEntryRow *entry_banner;
  AdwEntryRow *entry_nip05;
  AdwEntryRow *entry_lud16;
  AdwEntryRow *entry_website;
  GtkLabel *lbl_npub;

  /* Preview widgets */
  AdwPreferencesGroup *group_preview;
  AdwAvatar *preview_avatar;
  GtkLabel *preview_name;
  GtkLabel *preview_nip05;
  GtkLabel *preview_about;
  GtkLabel *preview_website;
  GtkLabel *preview_lud16;

  /* Status widgets */
  GtkBox *box_status;
  GtkSpinner *spinner_status;
  GtkLabel *lbl_status;

  /* State */
  gchar *npub;
  gboolean preview_visible;
  SheetProfileEditorSaveCb on_save;
  gpointer on_save_ud;
  SheetProfileEditorPublishCb on_publish;
  gpointer on_publish_ud;
};

G_DEFINE_TYPE(SheetProfileEditor, sheet_profile_editor, ADW_TYPE_DIALOG)

static void set_status(SheetProfileEditor *self, const gchar *message, gboolean spinning) {
  if (!self) return;

  if (message && *message) {
    if (self->lbl_status) gtk_label_set_text(self->lbl_status, message);
    if (self->spinner_status) gtk_spinner_set_spinning(self->spinner_status, spinning);
    if (self->box_status) gtk_widget_set_visible(GTK_WIDGET(self->box_status), TRUE);
  } else {
    if (self->box_status) gtk_widget_set_visible(GTK_WIDGET(self->box_status), FALSE);
    if (self->spinner_status) gtk_spinner_set_spinning(self->spinner_status, FALSE);
  }
}

static void on_cancel(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetProfileEditor *self = user_data;
  adw_dialog_close(ADW_DIALOG(self));
}

static void update_preview(SheetProfileEditor *self) {
  if (!self) return;

  const gchar *name = gtk_editable_get_text(GTK_EDITABLE(self->entry_name));
  const gchar *about = gtk_editable_get_text(GTK_EDITABLE(self->entry_about));
  const gchar *nip05 = gtk_editable_get_text(GTK_EDITABLE(self->entry_nip05));
  const gchar *website = gtk_editable_get_text(GTK_EDITABLE(self->entry_website));
  const gchar *lud16 = gtk_editable_get_text(GTK_EDITABLE(self->entry_lud16));

  /* Update preview labels */
  if (self->preview_name) {
    gtk_label_set_text(self->preview_name, (name && *name) ? name : "(No name)");
  }
  if (self->preview_about) {
    gtk_label_set_text(self->preview_about, (about && *about) ? about : "");
    gtk_widget_set_visible(GTK_WIDGET(self->preview_about), about && *about);
  }
  if (self->preview_nip05) {
    gtk_label_set_text(self->preview_nip05, (nip05 && *nip05) ? nip05 : "");
    gtk_widget_set_visible(GTK_WIDGET(self->preview_nip05), nip05 && *nip05);
  }
  if (self->preview_website) {
    gtk_label_set_text(self->preview_website, (website && *website) ? website : "");
    GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(self->preview_website));
    if (parent) gtk_widget_set_visible(parent, website && *website);
  }
  if (self->preview_lud16) {
    gtk_label_set_text(self->preview_lud16, (lud16 && *lud16) ? lud16 : "");
    GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(self->preview_lud16));
    if (parent) gtk_widget_set_visible(parent, lud16 && *lud16);
  }
  if (self->preview_avatar) {
    adw_avatar_set_text(self->preview_avatar, (name && *name) ? name : "?");
  }
}

static void on_preview(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetProfileEditor *self = user_data;
  if (!self) return;

  self->preview_visible = !self->preview_visible;

  if (self->preview_visible) {
    update_preview(self);
  }

  if (self->group_preview) {
    gtk_widget_set_visible(GTK_WIDGET(self->group_preview), self->preview_visible);
  }
  if (self->btn_preview) {
    gtk_button_set_label(self->btn_preview, self->preview_visible ? "Hide Preview" : "Preview");
  }
}

static gchar *build_profile_json(SheetProfileEditor *self) {
  const gchar *name = gtk_editable_get_text(GTK_EDITABLE(self->entry_name));
  const gchar *about = gtk_editable_get_text(GTK_EDITABLE(self->entry_about));
  const gchar *picture = gtk_editable_get_text(GTK_EDITABLE(self->entry_picture));
  const gchar *banner = gtk_editable_get_text(GTK_EDITABLE(self->entry_banner));
  const gchar *nip05 = gtk_editable_get_text(GTK_EDITABLE(self->entry_nip05));
  const gchar *lud16 = gtk_editable_get_text(GTK_EDITABLE(self->entry_lud16));
  const gchar *website = gtk_editable_get_text(GTK_EDITABLE(self->entry_website));

  /* Build content object */
  g_autoptr(JsonBuilder) content_builder = json_builder_new();
  json_builder_begin_object(content_builder);

  if (name && *name) {
    json_builder_set_member_name(content_builder, "name");
    json_builder_add_string_value(content_builder, name);
  }
  if (about && *about) {
    json_builder_set_member_name(content_builder, "about");
    json_builder_add_string_value(content_builder, about);
  }
  if (picture && *picture) {
    json_builder_set_member_name(content_builder, "picture");
    json_builder_add_string_value(content_builder, picture);
  }
  if (banner && *banner) {
    json_builder_set_member_name(content_builder, "banner");
    json_builder_add_string_value(content_builder, banner);
  }
  if (nip05 && *nip05) {
    json_builder_set_member_name(content_builder, "nip05");
    json_builder_add_string_value(content_builder, nip05);
  }
  if (lud16 && *lud16) {
    json_builder_set_member_name(content_builder, "lud16");
    json_builder_add_string_value(content_builder, lud16);
  }
  if (website && *website) {
    json_builder_set_member_name(content_builder, "website");
    json_builder_add_string_value(content_builder, website);
  }

  json_builder_end_object(content_builder);
  JsonNode *content_node = json_builder_get_root(content_builder);
  g_autoptr(JsonGenerator) content_gen = json_generator_new();
  json_generator_set_root(content_gen, content_node);
  gchar *content_str = json_generator_to_data(content_gen, NULL);
  json_node_unref(content_node);

  /* Build event */
  g_autoptr(JsonBuilder) event_builder = json_builder_new();
  json_builder_begin_object(event_builder);

  json_builder_set_member_name(event_builder, "kind");
  json_builder_add_int_value(event_builder, 0);

  json_builder_set_member_name(event_builder, "created_at");
  json_builder_add_int_value(event_builder, (gint64)time(NULL));

  json_builder_set_member_name(event_builder, "tags");
  json_builder_begin_array(event_builder);
  json_builder_end_array(event_builder);

  json_builder_set_member_name(event_builder, "content");
  json_builder_add_string_value(event_builder, content_str);

  json_builder_end_object(event_builder);

  JsonNode *event_node = json_builder_get_root(event_builder);
  g_autoptr(JsonGenerator) event_gen = json_generator_new();
  json_generator_set_root(event_gen, event_node);
  gchar *result = json_generator_to_data(event_gen, NULL);

  json_node_unref(event_node);
  g_free(content_str);

  return result;
}

/* Async publish completion data */
typedef struct {
  SheetProfileEditor *self;
  gchar *event_json;
  gchar *signed_event;
} PublishData;

static void publish_data_free(PublishData *pd) {
  if (!pd) return;
  g_free(pd->event_json);
  g_free(pd->signed_event);
  g_free(pd);
}

static gboolean publish_complete_idle(gpointer user_data) {
  PublishData *pd = user_data;
  if (!pd || !pd->self) {
    publish_data_free(pd);
    return G_SOURCE_REMOVE;
  }

  SheetProfileEditor *self = pd->self;

  /* Hide status */
  set_status(self, NULL, FALSE);

  /* Re-enable buttons */
  if (self->btn_save) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_save), TRUE);
  if (self->btn_cancel) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_cancel), TRUE);

  /* Notify via callback */
  if (self->on_save) {
    self->on_save(self->npub, pd->event_json, self->on_save_ud);
  }

  if (self->on_publish && pd->signed_event) {
    self->on_publish(self->npub, pd->signed_event, self->on_publish_ud);
  }

  adw_dialog_close(ADW_DIALOG(self));
  publish_data_free(pd);
  return G_SOURCE_REMOVE;
}

static void on_save(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetProfileEditor *self = user_data;
  if (!self) return;

  /* Build the unsigned event JSON */
  gchar *event_json = build_profile_json(self);
  if (!event_json) {
    GtkAlertDialog *ad = gtk_alert_dialog_new("Failed to build profile event");
    gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(ad);
    return;
  }

  /* Disable buttons and show progress */
  if (self->btn_save) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_save), FALSE);
  if (self->btn_cancel) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_cancel), FALSE);
  set_status(self, "Signing event...", TRUE);

  /* Sign the event using the signer's key management */
  gchar *signature = NULL;
  SecretStoreResult rc = secret_store_sign_event(event_json, self->npub, &signature);

  if (rc != SECRET_STORE_OK || !signature) {
    set_status(self, NULL, FALSE);
    if (self->btn_save) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_save), TRUE);
    if (self->btn_cancel) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_cancel), TRUE);

    const gchar *err_msg = "Failed to sign event";
    if (rc == SECRET_STORE_ERR_NOT_FOUND) {
      err_msg = "Key not found in secure storage";
    } else if (rc == SECRET_STORE_ERR_INVALID_KEY) {
      err_msg = "Invalid key format";
    }

    GtkAlertDialog *ad = gtk_alert_dialog_new("%s", err_msg);
    gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(ad);
    g_free(event_json);
    return;
  }

  set_status(self, "Publishing profile...", TRUE);

  /* Build signed event with signature */
  /* The signature from secret_store_sign_event is the full signed event JSON */
  gchar *signed_event = signature;

  /* Create publish data for completion callback */
  PublishData *pd = g_new0(PublishData, 1);
  pd->self = self;
  pd->event_json = event_json;
  pd->signed_event = signed_event;

  /* For now, simulate async completion with idle callback
   * In a full implementation, this would publish to relays first */
  g_idle_add(publish_complete_idle, pd);
}

static void on_entry_changed(AdwEntryRow *row, gpointer user_data) {
  (void)row;
  SheetProfileEditor *self = user_data;

  /* Enable save button if at least name is filled */
  const gchar *name = gtk_editable_get_text(GTK_EDITABLE(self->entry_name));
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_save), name && *name);
}

static void sheet_profile_editor_finalize(GObject *obj) {
  SheetProfileEditor *self = SHEET_PROFILE_EDITOR(obj);
  g_free(self->npub);
  G_OBJECT_CLASS(sheet_profile_editor_parent_class)->finalize(obj);
}

static void sheet_profile_editor_class_init(SheetProfileEditorClass *klass) {
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  GObjectClass *oc = G_OBJECT_CLASS(klass);

  oc->finalize = sheet_profile_editor_finalize;

  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/sheets/sheet-profile-editor.ui");

  /* Header buttons */
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, btn_cancel);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, btn_preview);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, btn_save);

  /* Form entries */
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, entry_name);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, entry_about);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, entry_picture);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, entry_banner);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, entry_nip05);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, entry_lud16);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, entry_website);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, lbl_npub);

  /* Preview widgets */
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, group_preview);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, preview_avatar);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, preview_name);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, preview_nip05);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, preview_about);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, preview_website);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, preview_lud16);

  /* Status widgets */
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, box_status);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, spinner_status);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, lbl_status);
}

static void on_entry_changed_for_preview(AdwEntryRow *row, gpointer user_data) {
  (void)row;
  SheetProfileEditor *self = user_data;
  if (self && self->preview_visible) {
    update_preview(self);
  }
}

static void sheet_profile_editor_init(SheetProfileEditor *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  self->preview_visible = FALSE;

  /* Connect button handlers */
  g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel), self);
  g_signal_connect(self->btn_preview, "clicked", G_CALLBACK(on_preview), self);
  g_signal_connect(self->btn_save, "clicked", G_CALLBACK(on_save), self);

  /* Monitor changes for save button enablement */
  g_signal_connect(self->entry_name, "changed", G_CALLBACK(on_entry_changed), self);

  /* Monitor changes for live preview updates */
  g_signal_connect(self->entry_name, "changed", G_CALLBACK(on_entry_changed_for_preview), self);
  g_signal_connect(self->entry_about, "changed", G_CALLBACK(on_entry_changed_for_preview), self);
  g_signal_connect(self->entry_nip05, "changed", G_CALLBACK(on_entry_changed_for_preview), self);
  g_signal_connect(self->entry_website, "changed", G_CALLBACK(on_entry_changed_for_preview), self);
  g_signal_connect(self->entry_lud16, "changed", G_CALLBACK(on_entry_changed_for_preview), self);

  /* Initially disable save */
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_save), FALSE);

  /* Focus name entry */
  gtk_widget_grab_focus(GTK_WIDGET(self->entry_name));
}

SheetProfileEditor *sheet_profile_editor_new(void) {
  return g_object_new(TYPE_SHEET_PROFILE_EDITOR, NULL);
}

void sheet_profile_editor_set_npub(SheetProfileEditor *self, const gchar *npub) {
  g_return_if_fail(self != NULL);

  g_free(self->npub);
  self->npub = g_strdup(npub);

  if (self->lbl_npub) {
    /* Show truncated npub */
    if (npub && strlen(npub) > 20) {
      gchar *display = g_strdup_printf("%.12s...%.6s", npub, npub + strlen(npub) - 6);
      gtk_label_set_text(self->lbl_npub, display);
      g_free(display);
    } else {
      gtk_label_set_text(self->lbl_npub, npub ? npub : "");
    }
  }
}

void sheet_profile_editor_set_on_save(SheetProfileEditor *self,
                                      SheetProfileEditorSaveCb cb,
                                      gpointer user_data) {
  g_return_if_fail(self != NULL);
  self->on_save = cb;
  self->on_save_ud = user_data;
}

void sheet_profile_editor_set_on_publish(SheetProfileEditor *self,
                                         SheetProfileEditorPublishCb cb,
                                         gpointer user_data) {
  g_return_if_fail(self != NULL);
  self->on_publish = cb;
  self->on_publish_ud = user_data;
}

void sheet_profile_editor_load_profile(SheetProfileEditor *self,
                                       const gchar *name,
                                       const gchar *about,
                                       const gchar *picture,
                                       const gchar *banner,
                                       const gchar *nip05,
                                       const gchar *lud16,
                                       const gchar *website) {
  g_return_if_fail(self != NULL);

  if (name) gtk_editable_set_text(GTK_EDITABLE(self->entry_name), name);
  if (about) gtk_editable_set_text(GTK_EDITABLE(self->entry_about), about);
  if (picture) gtk_editable_set_text(GTK_EDITABLE(self->entry_picture), picture);
  if (banner) gtk_editable_set_text(GTK_EDITABLE(self->entry_banner), banner);
  if (nip05) gtk_editable_set_text(GTK_EDITABLE(self->entry_nip05), nip05);
  if (lud16) gtk_editable_set_text(GTK_EDITABLE(self->entry_lud16), lud16);
  if (website) gtk_editable_set_text(GTK_EDITABLE(self->entry_website), website);

  /* Enable save if name is set */
  on_entry_changed(NULL, self);
}
