/* sheet-profile-editor.c - Profile editing dialog implementation */
#include "sheet-profile-editor.h"
#include "../app-resources.h"
#include "../../profile_store.h"

#include <json.h>
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
  json_object *content = json_object_new_object();

  const gchar *name = gtk_editable_get_text(GTK_EDITABLE(self->entry_name));
  const gchar *about = gtk_editable_get_text(GTK_EDITABLE(self->entry_about));
  const gchar *picture = gtk_editable_get_text(GTK_EDITABLE(self->entry_picture));
  const gchar *banner = gtk_editable_get_text(GTK_EDITABLE(self->entry_banner));
  const gchar *nip05 = gtk_editable_get_text(GTK_EDITABLE(self->entry_nip05));
  const gchar *lud16 = gtk_editable_get_text(GTK_EDITABLE(self->entry_lud16));
  const gchar *website = gtk_editable_get_text(GTK_EDITABLE(self->entry_website));

  if (name && *name) {
    json_object_object_add(content, "name", json_object_new_string(name));
  }
  if (about && *about) {
    json_object_object_add(content, "about", json_object_new_string(about));
  }
  if (picture && *picture) {
    json_object_object_add(content, "picture", json_object_new_string(picture));
  }
  if (banner && *banner) {
    json_object_object_add(content, "banner", json_object_new_string(banner));
  }
  if (nip05 && *nip05) {
    json_object_object_add(content, "nip05", json_object_new_string(nip05));
  }
  if (lud16 && *lud16) {
    json_object_object_add(content, "lud16", json_object_new_string(lud16));
  }
  if (website && *website) {
    json_object_object_add(content, "website", json_object_new_string(website));
  }

  const gchar *content_str = json_object_to_json_string(content);

  /* Build event */
  json_object *event = json_object_new_object();
  json_object_object_add(event, "kind", json_object_new_int(0));
  json_object_object_add(event, "created_at", json_object_new_int64((int64_t)time(NULL)));
  json_object_object_add(event, "tags", json_object_new_array());
  json_object_object_add(event, "content", json_object_new_string(content_str));

  gchar *result = g_strdup(json_object_to_json_string(event));

  json_object_put(content);
  json_object_put(event);

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
