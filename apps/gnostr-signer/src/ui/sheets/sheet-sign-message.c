/* sheet-sign-message.c - Sign arbitrary message dialog
 *
 * Allows user to sign arbitrary text messages with their Nostr private key.
 * Produces a Schnorr signature that can be verified against the public key.
 */

#include "sheet-sign-message.h"
#include "../app-resources.h"
#include <string.h>

struct _SheetSignMessage {
  AdwDialog parent_instance;

  /* Template children */
  GtkLabel *lbl_profile;
  GtkTextView *text_message;
  GtkRevealer *revealer_result;
  GtkLabel *lbl_signature;
  GtkButton *btn_copy_signature;
  GtkButton *btn_cancel;
  GtkButton *btn_sign;

  /* State */
  char *profile_name;
  char *account_id;
  char *signature_hex;
};

G_DEFINE_TYPE(SheetSignMessage, sheet_sign_message, ADW_TYPE_DIALOG)

static void on_text_buffer_changed(GtkTextBuffer *buffer, gpointer user_data);
static void on_btn_cancel_clicked(GtkButton *btn, gpointer user_data);
static void on_btn_sign_clicked(GtkButton *btn, gpointer user_data);
static void on_btn_copy_signature_clicked(GtkButton *btn, gpointer user_data);

static void
sheet_sign_message_dispose(GObject *object)
{
  SheetSignMessage *self = SHEET_SIGN_MESSAGE(object);
  
  g_clear_pointer(&self->profile_name, g_free);
  g_clear_pointer(&self->account_id, g_free);
  if (self->signature_hex) {
    memset(self->signature_hex, 0, strlen(self->signature_hex));
    g_free(self->signature_hex);
    self->signature_hex = NULL;
  }
  
  G_OBJECT_CLASS(sheet_sign_message_parent_class)->dispose(object);
}

static void
sheet_sign_message_class_init(SheetSignMessageClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = sheet_sign_message_dispose;

  gtk_widget_class_set_template_from_resource(widget_class,
    APP_RESOURCE_PATH "/ui/sheets/sheet-sign-message.ui");

  gtk_widget_class_bind_template_child(widget_class, SheetSignMessage, lbl_profile);
  gtk_widget_class_bind_template_child(widget_class, SheetSignMessage, text_message);
  gtk_widget_class_bind_template_child(widget_class, SheetSignMessage, revealer_result);
  gtk_widget_class_bind_template_child(widget_class, SheetSignMessage, lbl_signature);
  gtk_widget_class_bind_template_child(widget_class, SheetSignMessage, btn_copy_signature);
  gtk_widget_class_bind_template_child(widget_class, SheetSignMessage, btn_cancel);
  gtk_widget_class_bind_template_child(widget_class, SheetSignMessage, btn_sign);
}

static void
sheet_sign_message_init(SheetSignMessage *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));

  /* Connect signals */
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->text_message);
  g_signal_connect(buffer, "changed", G_CALLBACK(on_text_buffer_changed), self);
  g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_btn_cancel_clicked), self);
  g_signal_connect(self->btn_sign, "clicked", G_CALLBACK(on_btn_sign_clicked), self);
  g_signal_connect(self->btn_copy_signature, "clicked", G_CALLBACK(on_btn_copy_signature_clicked), self);
}

SheetSignMessage *
sheet_sign_message_new(GtkWindow *parent)
{
  SheetSignMessage *self = g_object_new(SHEET_TYPE_SIGN_MESSAGE, NULL);
  if (parent) {
    adw_dialog_present(ADW_DIALOG(self), GTK_WIDGET(parent));
  }
  return self;
}

void
sheet_sign_message_set_profile(SheetSignMessage *self, const char *profile_name, const char *account_id)
{
  g_return_if_fail(SHEET_IS_SIGN_MESSAGE(self));
  
  g_free(self->profile_name);
  g_free(self->account_id);
  
  self->profile_name = g_strdup(profile_name);
  self->account_id = g_strdup(account_id);
  
  if (profile_name) {
    gtk_label_set_text(self->lbl_profile, profile_name);
  }
}

static void
on_text_buffer_changed(GtkTextBuffer *buffer, gpointer user_data)
{
  SheetSignMessage *self = SHEET_SIGN_MESSAGE(user_data);
  
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  
  gboolean has_text = text && *text;
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_sign), has_text);
  
  g_free(text);
}

static void
on_btn_cancel_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  SheetSignMessage *self = SHEET_SIGN_MESSAGE(user_data);
  adw_dialog_close(ADW_DIALOG(self));
}

static void
on_btn_copy_signature_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  SheetSignMessage *self = SHEET_SIGN_MESSAGE(user_data);
  
  if (self->signature_hex) {
    GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(self));
    gdk_clipboard_set_text(clipboard, self->signature_hex);
  }
}

static void
on_btn_sign_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  SheetSignMessage *self = SHEET_SIGN_MESSAGE(user_data);

  if (!self->account_id) {
    g_warning("No account ID set for signing");
    return;
  }

  /* Get message text */
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->text_message);
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  char *message = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  
  if (!message || !*message) {
    g_free(message);
    return;
  }

  /* TODO: Implement message signing using secret-storage API
   * 1. Retrieve private key using gn_secret_storage_retrieve_key()
   * 2. Hash message with SHA256
   * 3. Sign with secp256k1 Schnorr signature
   * 4. Display signature in hex format
   * For now, show placeholder */
  
  g_free(message);
  
  /* Placeholder signature for demonstration */
  const char *placeholder_sig = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  
  g_free(self->signature_hex);
  self->signature_hex = g_strdup(placeholder_sig);
  
  gtk_label_set_text(self->lbl_signature, placeholder_sig);
  gtk_revealer_set_reveal_child(self->revealer_result, TRUE);
  
  /* Change sign button to "Sign Another" */
  gtk_button_set_label(self->btn_sign, "Sign Another");
}
