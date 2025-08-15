#include "sheet-import-key.h"
#include "../app-resources.h"

#include <gtk/gtk.h>
#include <adwaita.h>
#include <gio/gio.h>
#include <string.h>

struct _SheetImportKey {
  AdwDialog parent_instance;
  GtkButton *btn_cancel;
  GtkButton *btn_ok;
  GtkEntry *entry_secret;
  GtkEntry *entry_label;
  GtkCheckButton *chk_link_user;
  /* Success callback wiring */
  SheetImportKeySuccessCb on_success;
  gpointer on_success_ud;
};

G_DEFINE_TYPE(SheetImportKey, sheet_import_key, ADW_TYPE_DIALOG)

typedef struct {
  SheetImportKey *self;
  GtkWindow *parent;
} ImportCtx;

/* Helper used in validation and clipboard prefill */
static gboolean is_hex64(const char *s){
  if (!s) return FALSE; size_t n = strlen(s); if (n != 64) return FALSE;
  for (size_t i=0;i<n;i++){
    char c = s[i];
    if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) return FALSE;
  }
  return TRUE;
}

static void clipboard_text_got(GObject *src, GAsyncResult *res, gpointer user_data){
  SheetImportKey *self = user_data; if (!self || !self->entry_secret) return;
  g_autoptr(GError) err = NULL;
  char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, &err);
  if (err || !text) return;
  g_strstrip(text);
  if (g_str_has_prefix(text, "nsec1") || g_str_has_prefix(text, "ncrypt") || is_hex64(text)){
    gtk_editable_set_text(GTK_EDITABLE(self->entry_secret), text);
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_ok), TRUE);
  }
  g_free(text);
}

static void on_secret_changed(GtkEditable *e, gpointer user_data){
  SheetImportKey *self = (SheetImportKey*)user_data;
  if (!self) return;
  const char *t = gtk_editable_get_text(e);
  gboolean has = (t && *t);
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_ok), has);
}

static void import_call_done(GObject *src, GAsyncResult *res, gpointer user_data){
  (void)src;
  ImportCtx *ctx = (ImportCtx*)user_data;
  GError *err=NULL; GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
  gboolean ok = FALSE;
  g_autofree char *npub = NULL;
  if (err){
    const char *domain = g_quark_to_string(err->domain);
    g_warning("StoreKey DBus error: [%s] code=%d msg=%s", domain?domain:"?", err->code, err->message);
    GtkAlertDialog *ad = gtk_alert_dialog_new("Import failed: %s (%s:%d)", err->message, domain?domain:"?", err->code);
    gtk_alert_dialog_show(ad, ctx && ctx->parent ? ctx->parent : GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(ctx->self))));
    g_object_unref(ad);
    g_clear_error(&err);
  } else if (ret){
    /* Expect (b,s): ok, npub. Duplicate string before unref */
    const char *npub_in = NULL;
    g_variant_get(ret, "(bs)", &ok, &npub_in);
    if (npub_in) npub = g_strdup(npub_in);
    g_variant_unref(ret);
    g_message("StoreKey reply ok=%s npub='%s'", ok?"true":"false", (npub&&*npub)?npub:"(empty)");
    if (ok){
      /* Fallback: if npub wasn't returned, query active public key */
      if (!(npub && *npub)){
        GError *e2=NULL; GDBusConnection *bus2 = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &e2);
        if (bus2){
          GVariant *ret2 = g_dbus_connection_call_sync(bus2,
                               "org.nostr.Signer",
                               "/org/nostr/signer",
                               "org.nostr.Signer",
                               "GetPublicKey",
                               NULL,
                               G_VARIANT_TYPE("(s)"),
                               G_DBUS_CALL_FLAGS_NONE,
                               2000,
                               NULL,
                               &e2);
          if (ret2){
            const char *np=NULL; g_variant_get(ret2, "(s)", &np);
            if (np && *np) { if (npub) g_free(npub); npub = g_strdup(np); }
            g_variant_unref(ret2);
          }
          g_object_unref(bus2);
        }
        if (e2){ g_clear_error(&e2); }
      }
      const char *npub_show = (npub && *npub) ? npub : "(npub unavailable)";
      GtkAlertDialog *ad = gtk_alert_dialog_new("Account added and set active for %s\n(npub copied to clipboard)", npub_show);
      gtk_alert_dialog_show(ad, ctx && ctx->parent ? ctx->parent : GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(ctx->self))));
      g_object_unref(ad);
      /* Copy npub to clipboard for convenience */
      if (npub && *npub) {
        GtkWidget *w = GTK_WIDGET(ctx->self);
        GdkDisplay *dpy = gtk_widget_get_display(w);
        if (dpy){ GdkClipboard *cb = gdk_display_get_clipboard(dpy); if (cb) gdk_clipboard_set_text(cb, npub); }
      }
      /* Notify parent to update AccountsStore and refresh UI */
      if (ctx && ctx->self && ctx->self->on_success) {
        const char *label = NULL;
        if (ctx->self->entry_label) label = gtk_editable_get_text(GTK_EDITABLE(ctx->self->entry_label));
        ctx->self->on_success(npub ? npub : "", label ? label : "", ctx->self->on_success_ud);
      }
    } else {
      /* Log more diagnostics client-side */
      const char *entered = gtk_editable_get_text(GTK_EDITABLE(ctx->self->entry_secret));
      const char *kind = entered && g_str_has_prefix(entered, "nsec1") ? "nsec" : (entered && g_str_has_prefix(entered, "ncrypt") ? "ncrypt" : "hex/other");
      g_message("StoreKey returned ok=false. input_kind=%s len=%zu", kind, entered ? strlen(entered) : 0ul);
      const char *hint = "\n\nHints:\n• Ensure the daemon was started with NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1\n• Verify the key is a valid nsec..., 64-hex, or ncrypt...";
      GtkAlertDialog *ad = gtk_alert_dialog_new("Import failed.%s", hint);
      gtk_alert_dialog_show(ad, ctx && ctx->parent ? ctx->parent : GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(ctx->self))));
      g_object_unref(ad);
      /* Keep dialog open for correction */
      /* Re-enable buttons */
      if (ctx->self->btn_ok) gtk_widget_set_sensitive(GTK_WIDGET(ctx->self->btn_ok), TRUE);
      if (ctx->self->btn_cancel) gtk_widget_set_sensitive(GTK_WIDGET(ctx->self->btn_cancel), TRUE);
      return;
    }
  }
  if (ctx && ctx->self) adw_dialog_close(ADW_DIALOG(ctx->self));
  g_free(ctx);
}

static void on_cancel(GtkButton *b, gpointer user_data){ (void)b; SheetImportKey *self = user_data; if (self) adw_dialog_close(ADW_DIALOG(self)); }

static void on_ok(GtkButton *b, gpointer user_data){
  (void)b;
  SheetImportKey *self = (SheetImportKey*)user_data;
  if (!self) return;
  const char *raw = gtk_editable_get_text(GTK_EDITABLE(self->entry_secret));
  if (!raw || *raw == '\0') { return; }
  g_autofree char *secret = g_strdup(raw);
  g_strstrip(secret);
  /* Basic validation: accept nsec..., ncrypt..., or 64-hex */
  if (!(g_str_has_prefix(secret, "nsec1") || g_str_has_prefix(secret, "ncrypt") || is_hex64(secret))){
    GtkAlertDialog *ad = gtk_alert_dialog_new("Invalid key format. Enter nsec..., 64-hex, or ncrypt...");
    gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(ad);
    return;
  }
  /* Identity optional: pass empty string; backend will derive npub if needed */
  const char *identity = "";
  /* Optionally could use chk_link_user in the future; current DBus has no flag */

  GError *e=NULL; GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &e);
  if (!bus){
    GtkAlertDialog *ad = gtk_alert_dialog_new("Failed to get session bus: %s", e?e->message:"unknown");
    gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    g_object_unref(ad);
    if (e) g_clear_error(&e);
    return;
  }
  ImportCtx *ctx = g_new0(ImportCtx, 1);
  ctx->self = self;
  ctx->parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self)));
  g_dbus_connection_call(bus,
                         "org.nostr.Signer",
                         "/org/nostr/signer",
                         "org.nostr.Signer",
                         "StoreKey",
                         g_variant_new("(ss)", secret, identity),
                         G_VARIANT_TYPE("(bs)"),
                         G_DBUS_CALL_FLAGS_NONE,
                         5000,
                         NULL,
                         import_call_done,
                         ctx);
  /* Disable buttons while request is in-flight */
  if (self->btn_ok) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_ok), FALSE);
  if (self->btn_cancel) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_cancel), FALSE);
  g_object_unref(bus);
}

static void sheet_import_key_class_init(SheetImportKeyClass *klass){
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/sheets/sheet-import-key.ui");
  gtk_widget_class_bind_template_child(wc, SheetImportKey, btn_cancel);
  gtk_widget_class_bind_template_child(wc, SheetImportKey, btn_ok);
  gtk_widget_class_bind_template_child(wc, SheetImportKey, entry_secret);
  gtk_widget_class_bind_template_child(wc, SheetImportKey, entry_label);
  gtk_widget_class_bind_template_child(wc, SheetImportKey, chk_link_user);
}

static void sheet_import_key_init(SheetImportKey *self){
  gtk_widget_init_template(GTK_WIDGET(self));
  if (self->btn_cancel) g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel), self);
  if (self->btn_ok) g_signal_connect(self->btn_ok, "clicked", G_CALLBACK(on_ok), self);
  if (self->entry_secret) gtk_widget_grab_focus(GTK_WIDGET(self->entry_secret));
  if (self->entry_secret) g_signal_connect(self->entry_secret, "changed", G_CALLBACK(on_secret_changed), self);
  if (self->btn_ok) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_ok), FALSE);
  /* Prefill from clipboard if it looks like a key */
  GtkWidget *w = GTK_WIDGET(self);
  GdkDisplay *dpy = gtk_widget_get_display(w);
  if (dpy){ GdkClipboard *cb = gdk_display_get_clipboard(dpy);
    if (cb) gdk_clipboard_read_text_async(cb, NULL, clipboard_text_got, self);
  }
}

SheetImportKey *sheet_import_key_new(void){ return g_object_new(TYPE_SHEET_IMPORT_KEY, NULL); }

void sheet_import_key_set_on_success(SheetImportKey *self,
                                     SheetImportKeySuccessCb cb,
                                     gpointer user_data){
  if (!self) return;
  self->on_success = cb;
  self->on_success_ud = user_data;
}
