#include "sheet-create-bunker.h"
#include "sheet-qr-display.h"
#include "../app-resources.h"
#include "../../accounts_store.h"
#include "../../settings_manager.h"
#include "nostr/nip55l/signer_ops.h"
#include "nostr/nip19/nip19.h"
#include <stdlib.h>
#include <string.h>

struct _SheetCreateBunker { AdwDialog parent_instance; GtkButton *btn_cancel; GtkButton *btn_create; GtkEntry *entry_name; GtkEntry *entry_relay; };
G_DEFINE_TYPE(SheetCreateBunker, sheet_create_bunker, ADW_TYPE_DIALOG)

static void sheet_create_bunker_class_init(SheetCreateBunkerClass *klass){
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/sheets/sheet-create-bunker.ui");
  gtk_widget_class_bind_template_child(wc, SheetCreateBunker, btn_cancel);
  gtk_widget_class_bind_template_child(wc, SheetCreateBunker, btn_create);
  gtk_widget_class_bind_template_child(wc, SheetCreateBunker, entry_name);
  gtk_widget_class_bind_template_child(wc, SheetCreateBunker, entry_relay);
}

static void show_error(SheetCreateBunker *self, const char *msg){
  GtkAlertDialog *dlg = gtk_alert_dialog_new("%s", msg);
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  gtk_alert_dialog_show(dlg, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL);
  g_object_unref(dlg);
}

/* Normalises one relay URL the same way the signer's GetRelays does.
 * NULL if it is not a ws:// or wss:// URL. */
static gchar *normalize_relay(const char *url){
  const char *one[] = { url };
  char *js = NULL;
  if (nostr_nip55l_relays_from_list(one, 1, &js) != 0 || !js) return NULL;
  /* A single-entry result is exactly ["<url>"] with no escapes. */
  size_t n = strlen(js);
  gchar *out = (n > 4) ? g_strndup(js + 2, n - 4) : NULL;
  free(js);
  return out;
}

static gchar *npub_to_hex(const char *npub){
  uint8_t pk[32];
  if (!npub || nostr_nip19_decode_npub(npub, pk) != 0) return NULL;
  GString *hex = g_string_sized_new(64);
  for (int i = 0; i < 32; i++) g_string_append_printf(hex, "%02x", pk[i]);
  return g_string_free(hex, FALSE);
}

/* Pairing: record the relay in the bunker settings the daemon's --bunker
 * mode serves from, then show the bunker:// URI a client scans.
 *
 * No secret= parameter: the NIP-46 bunker does not validate connect secrets
 * yet, so one here would imply a check that does not happen. Clients are
 * admitted by bunker-allowed-pubkeys instead. */
static void on_create(GtkButton *b, gpointer user_data){
  (void)b;
  SheetCreateBunker *self = user_data;
  if (!self) return;

  const char *relay_in = self->entry_relay ? gtk_editable_get_text(GTK_EDITABLE(self->entry_relay)) : NULL;
  g_autofree gchar *relay_trim = g_strstrip(g_strdup(relay_in ? relay_in : ""));
  g_autofree gchar *relay = normalize_relay(relay_trim);
  if (!relay) {
    show_error(self, "Enter a relay URL starting with wss:// (or ws:// for a local relay).");
    return;
  }

  g_autofree gchar *npub = NULL;
  if (!accounts_store_get_active(accounts_store_get_default(), &npub, NULL) || !npub || !*npub) {
    show_error(self, "Select or create an identity before creating a bunker.");
    return;
  }
  g_autofree gchar *pk_hex = npub_to_hex(npub);
  if (!pk_hex) {
    show_error(self, "The active identity is not a valid npub.");
    return;
  }

  SettingsManager *sm = settings_manager_get_default();
  g_auto(GStrv) relays = settings_manager_get_bunker_relays(sm);
  GPtrArray *merged = g_ptr_array_new_with_free_func(g_free);
  if (relays) for (guint i = 0; relays[i]; i++) g_ptr_array_add(merged, g_strdup(relays[i]));
  if (!relays || !g_strv_contains((const gchar *const *)relays, relay)) g_ptr_array_add(merged, g_strdup(relay));
  g_ptr_array_add(merged, NULL);
  settings_manager_set_bunker_relays(sm, (const gchar *const *)merged->pdata);
  settings_manager_set_bunker_enabled(sm, TRUE);

  GString *uri = g_string_new("bunker://");
  g_string_append(uri, pk_hex);
  for (guint i = 0; i + 1 < merged->len; i++) {
    g_autofree gchar *esc = g_uri_escape_string(g_ptr_array_index(merged, i), NULL, FALSE);
    g_string_append_printf(uri, "%srelay=%s", i == 0 ? "?" : "&", esc);
  }
  g_ptr_array_unref(merged);

  SheetQrDisplay *qr = sheet_qr_display_new();
  sheet_qr_display_set_bunker_uri(qr, uri->str);
  g_string_free(uri, TRUE);

  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  adw_dialog_close(ADW_DIALOG(self));
  adw_dialog_present(ADW_DIALOG(qr), GTK_IS_WIDGET(root) ? GTK_WIDGET(root) : NULL);
}

static void on_cancel(GtkButton *b, gpointer user_data){ (void)b; SheetCreateBunker *self = user_data; if (self) adw_dialog_close(ADW_DIALOG(self)); }
static void on_entry_activate(GtkEntry *e, gpointer user_data){ (void)e; SheetCreateBunker *self = user_data; if (self && self->btn_create) gtk_widget_activate(GTK_WIDGET(self->btn_create)); }
static void sheet_create_bunker_init(SheetCreateBunker *self){
  gtk_widget_init_template(GTK_WIDGET(self));
  if (self->btn_cancel) g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel), self);
  if (self->btn_create) g_signal_connect(self->btn_create, "clicked", G_CALLBACK(on_create), self);
  if (self->entry_name) {
    g_signal_connect(self->entry_name, "activate", G_CALLBACK(on_entry_activate), self);
    gtk_widget_grab_focus(GTK_WIDGET(self->entry_name));
  }
  if (self->entry_relay) g_signal_connect(self->entry_relay, "activate", G_CALLBACK(on_entry_activate), self);
}

SheetCreateBunker *sheet_create_bunker_new(void){ return g_object_new(TYPE_SHEET_CREATE_BUNKER, NULL); }
