/* sheet-profile-editor.c - Profile editing dialog implementation */
#include "sheet-profile-editor.h"
#include "../app-resources.h"
#include "../../profile_store.h"

#include <json-glib/json-glib.h>
#include <time.h>

struct _SheetProfileEditor {
  AdwDialog parent_instance;

  /* Template children */
  GtkButton *btn_cancel;
  GtkButton *btn_save;
  AdwEntryRow *entry_name;
  AdwEntryRow *entry_about;
  AdwEntryRow *entry_picture;
  AdwEntryRow *entry_banner;
  AdwEntryRow *entry_nip05;
  AdwEntryRow *entry_lud16;
  AdwEntryRow *entry_website;
  GtkLabel *lbl_npub;

  /* State */
  gchar *npub;
  SheetProfileEditorSaveCb on_save;
  gpointer on_save_ud;
};

G_DEFINE_TYPE(SheetProfileEditor, sheet_profile_editor, ADW_TYPE_DIALOG)

static void on_cancel(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetProfileEditor *self = user_data;
  adw_dialog_close(ADW_DIALOG(self));
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
  JsonBuilder *content_builder = json_builder_new();
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
  JsonGenerator *content_gen = json_generator_new();
  json_generator_set_root(content_gen, content_node);
  gchar *content_str = json_generator_to_data(content_gen, NULL);
  g_object_unref(content_gen);
  json_node_unref(content_node);
  g_object_unref(content_builder);

  /* Build event */
  JsonBuilder *event_builder = json_builder_new();
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
  JsonGenerator *event_gen = json_generator_new();
  json_generator_set_root(event_gen, event_node);
  gchar *result = json_generator_to_data(event_gen, NULL);

  g_object_unref(event_gen);
  json_node_unref(event_node);
  g_object_unref(event_builder);
  g_free(content_str);

  return result;
}

static void on_save(GtkButton *btn, gpointer user_data) {
  (void)btn;
  SheetProfileEditor *self = user_data;

  if (self->on_save) {
    gchar *event_json = build_profile_json(self);
    self->on_save(self->npub, event_json, self->on_save_ud);
    g_free(event_json);
  }

  adw_dialog_close(ADW_DIALOG(self));
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
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, btn_cancel);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, btn_save);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, entry_name);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, entry_about);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, entry_picture);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, entry_banner);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, entry_nip05);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, entry_lud16);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, entry_website);
  gtk_widget_class_bind_template_child(wc, SheetProfileEditor, lbl_npub);
}

static void sheet_profile_editor_init(SheetProfileEditor *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel), self);
  g_signal_connect(self->btn_save, "clicked", G_CALLBACK(on_save), self);

  /* Monitor changes */
  g_signal_connect(self->entry_name, "changed", G_CALLBACK(on_entry_changed), self);

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
