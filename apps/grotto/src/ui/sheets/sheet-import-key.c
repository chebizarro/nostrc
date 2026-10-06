#include "sheet-import-key.h"
#include "sheet-qr-scanner.h"
#include "../app-resources.h"
#include "../../secure-delete.h"
#include "../../keyboard-nav.h"
#include "../../qr-code.h"

#include <gtk/gtk.h>
#include <adwaita.h>
#include <gio/gio.h>
#include <string.h>
/* A key store/create may wait on a keyring prompt (unlock, or the first
 * keyring being created); it must not time out under the person (W32). */
#ifndef GN_KEY_CALL_TIMEOUT_MS
#define GN_KEY_CALL_TIMEOUT_MS (10 * 60 * 1000)
#endif

/* Clipboard clear timeout in seconds after importing sensitive data */
#define CLIPBOARD_CLEAR_TIMEOUT_SECONDS 30

struct _SheetImportKey {
  AdwDialog parent_instance;
  GtkButton *btn_cancel;
  GtkButton *btn_ok;
  GtkButton *btn_scan_qr;
  GtkEntry *entry_secret;
  GtkEntry *entry_label;
  GtkCheckButton *chk_link_user;
  /* Success callback wiring */
  SheetImportKeySuccessCb on_success;
  gpointer on_success_ud;
  /* The QR scanner sheet can outlive this dialog; weak, cleared on
   * dispose (nostrc-wcxr). */
  SheetQrScanner *scanner;
  /* Cancels the clipboard prefill read when the dialog is closed */
  GCancellable *cancellable;
};

G_DEFINE_TYPE(SheetImportKey, sheet_import_key, ADW_TYPE_DIALOG)

typedef struct {
  SheetImportKey *self;   /* strong ref for the D-Bus call (nostrc-afg5) */
  GtkWindow *parent;      /* strong ref */
} ImportCtx;

static void import_ctx_free(ImportCtx *ctx) {
  if (!ctx) return;
  g_clear_object(&ctx->self);
  g_clear_object(&ctx->parent);
  g_free(ctx);
}

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
  SheetImportKey *self = user_data;  /* strong ref taken at init (nostrc-afg5) */
  g_autoptr(GError) err = NULL;
  char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(src), res, &err);
  if (err || !text) {
    g_object_unref(self);
    return;
  }
  g_strstrip(text);
  if (self->entry_secret &&
      (g_str_has_prefix(text, "nsec1") || g_str_has_prefix(text, "ncrypt") || is_hex64(text))){
    gtk_editable_set_text(GTK_EDITABLE(self->entry_secret), text);
    if (self->btn_ok) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_ok), TRUE);

    /* Schedule clipboard clear for security - don't leave secret keys on clipboard */
    gn_clipboard_clear_after(GDK_CLIPBOARD(src), CLIPBOARD_CLEAR_TIMEOUT_SECONDS);
  }
  /* Securely shred the text buffer before freeing */
  gn_secure_shred_string(text);
  g_free(text);
  g_object_unref(self);
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
    g_autoptr(GtkAlertDialog) ad = gtk_alert_dialog_new("Import failed: %s (%s:%d)", err->message, domain?domain:"?", err->code);
    gtk_alert_dialog_show(ad, ctx && ctx->parent ? ctx->parent : GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(ctx->self))));
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
        GError *e2=NULL; g_autoptr(GDBusConnection) bus2 = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &e2);
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
        }
        if (e2){ g_clear_error(&e2); }
      }
      const char *npub_show = (npub && *npub) ? npub : "(npub unavailable)";
      g_autoptr(GtkAlertDialog) ad = gtk_alert_dialog_new("Account added and set active for %s\n(npub copied to clipboard)", npub_show);
      gtk_alert_dialog_show(ad, ctx && ctx->parent ? ctx->parent : GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(ctx->self))));
      /* Copy npub to clipboard for convenience */
      if (npub && *npub) {
        GtkWidget *w = GTK_WIDGET(ctx->self);
        GdkDisplay *dpy = gtk_widget_get_display(w);
        if (dpy){ GdkClipboard *cb = gdk_display_get_clipboard(dpy); if (cb) gdk_clipboard_set_text(cb, npub); }
      }
      /* Notify parent to update AccountsStore and refresh UI.
       * entry_label is a template child: NULL after the dialog was
       * disposed during the in-flight call (nostrc-afg5). */
      if (ctx && ctx->self && ctx->self->on_success) {
        const char *label = NULL;
        if (ctx->self->entry_label) label = gtk_editable_get_text(GTK_EDITABLE(ctx->self->entry_label));
        ctx->self->on_success(npub ? npub : "", label ? label : "", ctx->self->on_success_ud);
      }
    } else {
      /* Log more diagnostics client-side */
      const char *entered = ctx->self->entry_secret
          ? gtk_editable_get_text(GTK_EDITABLE(ctx->self->entry_secret)) : NULL;
      const char *kind = entered && g_str_has_prefix(entered, "nsec1") ? "nsec" : (entered && g_str_has_prefix(entered, "ncrypt") ? "ncrypt" : "hex/other");
      g_message("StoreKey returned ok=false. input_kind=%s len=%zu", kind, entered ? strlen(entered) : 0ul);
      const char *hint = "\n\nHints:\n\u2022 Verify the key is a valid nsec..., 64-hex, or ncrypt...\n\u2022 Only the installed Grotto may store keys; one run from a build directory needs the daemon started with NOSTR_SIGNER_ALLOW_KEY_MUTATIONS=1";
      g_autoptr(GtkAlertDialog) ad = gtk_alert_dialog_new("Import failed.%s", hint);
      gtk_alert_dialog_show(ad, ctx && ctx->parent ? ctx->parent : GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(ctx->self))));
      /* Keep dialog open for correction */
      /* Re-enable buttons */
      if (ctx->self->btn_ok) gtk_widget_set_sensitive(GTK_WIDGET(ctx->self->btn_ok), TRUE);
      if (ctx->self->btn_cancel) gtk_widget_set_sensitive(GTK_WIDGET(ctx->self->btn_cancel), TRUE);
      import_ctx_free(ctx);
      return;
    }
  }
  if (ctx && ctx->self) adw_dialog_close(ADW_DIALOG(ctx->self));
  import_ctx_free(ctx);
}

static void on_cancel(GtkButton *b, gpointer user_data){ (void)b; SheetImportKey *self = user_data; if (self) adw_dialog_close(ADW_DIALOG(self)); }

/* QR scanner success callback */
static void on_qr_scan_success(const gchar *data, GnQrContentType type, gpointer user_data) {
  SheetImportKey *self = SHEET_IMPORT_KEY(user_data);
  if (!self || !data) return;

  (void)type; /* Type is already validated by scanner */

  /* Extract key data from nostr: URI if needed */
  const gchar *key_data = data;
  if (g_str_has_prefix(data, "nostr:")) {
    key_data = data + 6; /* Skip "nostr:" prefix */
  }

  /* Set the scanned data in the entry field */
  if (self->entry_secret) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_secret), key_data);
    /* Enable the OK button */
    if (self->btn_ok) {
      gtk_widget_set_sensitive(GTK_WIDGET(self->btn_ok), TRUE);
    }
  }
}

/* Handler: Scan QR code button */
static void on_scan_qr(GtkButton *b, gpointer user_data) {
  (void)b;
  SheetImportKey *self = SHEET_IMPORT_KEY(user_data);
  if (!self) return;

  /* Create and show QR scanner dialog */
  SheetQrScanner *scanner = sheet_qr_scanner_new();
  sheet_qr_scanner_set_on_success(scanner, on_qr_scan_success, self);

  /* nostrc-wcxr: the scanner can outlive this sheet. Track it weakly and
   * clear its callback from our dispose so it never calls into freed
   * state. */
  if (self->scanner) {
    sheet_qr_scanner_set_on_success(self->scanner, NULL, NULL);
    g_clear_weak_pointer(&self->scanner);
  }
  g_set_weak_pointer(&self->scanner, scanner);

  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  GtkWidget *parent = root ? GTK_WIDGET(root) : GTK_WIDGET(self);
  adw_dialog_present(ADW_DIALOG(scanner), parent);
}

static void on_ok(GtkButton *b, gpointer user_data){
  (void)b;
  SheetImportKey *self = (SheetImportKey*)user_data;
  if (!self) return;
  const char *raw = gtk_editable_get_text(GTK_EDITABLE(self->entry_secret));
  if (!raw || *raw == '\0') { return; }
  char *secret = g_strdup(raw);
  g_strstrip(secret);
  /* Basic validation: accept nsec..., ncrypt..., or 64-hex */
  if (!(g_str_has_prefix(secret, "nsec1") || g_str_has_prefix(secret, "ncrypt") || is_hex64(secret))){
    g_autoptr(GtkAlertDialog) ad = gtk_alert_dialog_new("Invalid key format. Enter nsec..., 64-hex, or ncrypt...");
    gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    /* Securely shred the secret before freeing */
    gn_secure_shred_string(secret);
    g_free(secret);
    return;
  }
  /* Identity optional: pass empty string; backend will derive npub if needed */
  const char *identity = "";
  /* Optionally could use chk_link_user in the future; current DBus has no flag */

  GError *e=NULL; g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &e);
  if (!bus){
    g_autoptr(GtkAlertDialog) ad = gtk_alert_dialog_new("Failed to get session bus: %s", e?e->message:"unknown");
    gtk_alert_dialog_show(ad, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self))));
    if (e) g_clear_error(&e);
    /* Securely shred the secret before freeing */
    gn_secure_shred_string(secret);
    g_free(secret);
    return;
  }
  ImportCtx *ctx = g_new0(ImportCtx, 1);
  ctx->self = g_object_ref(self);
  ctx->parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self)));
  if (ctx->parent) g_object_ref(ctx->parent);
  g_dbus_connection_call(bus,
                         "org.nostr.Signer",
                         "/org/nostr/signer",
                         "org.nostr.Signer",
                         "StoreKey",
                         g_variant_new("(ss)", secret, identity),
                         G_VARIANT_TYPE("(bs)"),
                         G_DBUS_CALL_FLAGS_NONE,
                         GN_KEY_CALL_TIMEOUT_MS,
                         NULL,
                         import_call_done,
                         ctx);

  /* Securely shred the secret after sending - DBus has made its copy */
  gn_secure_shred_string(secret);
  g_free(secret);

  /* Clear the entry field to remove secret from UI memory */
  gtk_editable_set_text(GTK_EDITABLE(self->entry_secret), "");

  /* Disable buttons while request is in-flight */
  if (self->btn_ok) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_ok), FALSE);
  if (self->btn_cancel) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_cancel), FALSE);
}

static void sheet_import_key_dispose(GObject *object){
  SheetImportKey *self = SHEET_IMPORT_KEY(object);

  /* nostrc-wcxr: detach the outliving QR scanner's callback */
  if (self->scanner) {
    sheet_qr_scanner_set_on_success(self->scanner, NULL, NULL);
    g_clear_weak_pointer(&self->scanner);
  }
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }

  G_OBJECT_CLASS(sheet_import_key_parent_class)->dispose(object);
}

static void sheet_import_key_class_init(SheetImportKeyClass *klass){
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose = sheet_import_key_dispose;
  GtkWidgetClass *wc = GTK_WIDGET_CLASS(klass);
  gtk_widget_class_set_template_from_resource(wc, APP_RESOURCE_PATH "/ui/sheets/sheet-import-key.ui");
  gtk_widget_class_bind_template_child(wc, SheetImportKey, btn_cancel);
  gtk_widget_class_bind_template_child(wc, SheetImportKey, btn_ok);
  gtk_widget_class_bind_template_child(wc, SheetImportKey, btn_scan_qr);
  gtk_widget_class_bind_template_child(wc, SheetImportKey, entry_secret);
  gtk_widget_class_bind_template_child(wc, SheetImportKey, entry_label);
  gtk_widget_class_bind_template_child(wc, SheetImportKey, chk_link_user);
}

static void sheet_import_key_init(SheetImportKey *self){
  gtk_widget_init_template(GTK_WIDGET(self));
  self->cancellable = g_cancellable_new();
  if (self->btn_cancel) g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_cancel), self);
  if (self->btn_ok) g_signal_connect(self->btn_ok, "clicked", G_CALLBACK(on_ok), self);
  if (self->btn_scan_qr) g_signal_connect(self->btn_scan_qr, "clicked", G_CALLBACK(on_scan_qr), self);
  if (self->entry_secret) g_signal_connect(self->entry_secret, "changed", G_CALLBACK(on_secret_changed), self);
  if (self->btn_ok) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_ok), FALSE);

  /* Setup keyboard navigation: focus entry on dialog open, Enter activates Add button */
  gn_keyboard_nav_setup_dialog(ADW_DIALOG(self),
                                GTK_WIDGET(self->entry_secret),
                                GTK_WIDGET(self->btn_ok));

  /* Connect Enter key in entry to activate button */
  if (self->entry_secret && self->btn_ok) {
    gn_keyboard_nav_connect_enter_activate(GTK_WIDGET(self->entry_secret),
                                            GTK_WIDGET(self->btn_ok));
  }

  /* Prefill from clipboard if it looks like a key (self reffed until the
   * read completes; the cancellable fires on dispose — nostrc-afg5) */
  GtkWidget *w = GTK_WIDGET(self);
  GdkDisplay *dpy = gtk_widget_get_display(w);
  if (dpy){ GdkClipboard *cb = gdk_display_get_clipboard(dpy);
    if (cb) gdk_clipboard_read_text_async(cb, self->cancellable, clipboard_text_got, g_object_ref(self));
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
