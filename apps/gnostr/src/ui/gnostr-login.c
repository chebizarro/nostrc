/**
 * GnostrLogin - Login Dialog for NIP-55L and NIP-46 Authentication
 *
 * Provides sign-in options:
 * 1. NIP-55L: Local signer via D-Bus (gnostr-signer)
 * 2. NIP-46: Remote signer via bunker:// URI with QR code display
 */

#include "gnostr-login.h"
#include "../ipc/signer_ipc.h"
#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_uri.h"
#include "nostr/nip19/nip19.h"
#include <glib/gi18n.h>
#include <jansson.h>

/* QR code generation - using qrencode if available */
#ifdef HAVE_QRENCODE
#include <qrencode.h>
#endif

#define UI_RESOURCE "/org/gnostr/ui/ui/dialogs/gnostr-login.ui"

/* Settings schema and key names */
#define SETTINGS_SCHEMA_CLIENT "org.gnostr.Client"
#define SETTINGS_KEY_CURRENT_NPUB "current-npub"

struct _GnostrLogin {
  GtkWindow parent_instance;

  /* Template children */
  GtkWidget *stack;
  GtkWidget *page_choose;
  GtkWidget *page_bunker;
  GtkWidget *page_success;

  /* Choose page widgets */
  GtkWidget *lbl_local_status;
  GtkWidget *btn_local_signer;
  GtkWidget *spinner_local;
  GtkWidget *btn_remote_signer;

  /* Bunker page widgets */
  GtkWidget *qr_frame;
  GtkWidget *qr_picture;
  GtkWidget *entry_bunker_uri;
  GtkWidget *btn_paste_bunker;
  GtkWidget *btn_connect_bunker;
  GtkWidget *spinner_bunker;
  GtkWidget *lbl_bunker_status;
  GtkWidget *btn_back_bunker;

  /* Success page widgets */
  GtkWidget *lbl_success_npub;
  GtkWidget *btn_done;

  /* Toast */
  GtkWidget *toast_revealer;
  GtkWidget *toast_label;

  /* State */
  gboolean checking_local;
  gboolean connecting_local;
  gboolean connecting_bunker;
  gboolean local_signer_available;
  char *nostrconnect_uri;          /* URI for QR code display */
  char *nostrconnect_secret;       /* Secret for bunker auth */
  NostrNip46Session *nip46_session; /* NIP-46 session */
  GCancellable *cancellable;       /* For async operations */
};

G_DEFINE_TYPE(GnostrLogin, gnostr_login, GTK_TYPE_WINDOW)

/* Signals */
enum {
  SIGNAL_SIGNED_IN,
  SIGNAL_CANCELLED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

/* Forward declarations */
static void on_local_signer_clicked(GtkButton *btn, gpointer user_data);
static void on_remote_signer_clicked(GtkButton *btn, gpointer user_data);
static void on_back_clicked(GtkButton *btn, gpointer user_data);
static void on_paste_bunker_clicked(GtkButton *btn, gpointer user_data);
static void on_connect_bunker_clicked(GtkButton *btn, gpointer user_data);
static void on_close_clicked(GtkButton *btn, gpointer user_data);
static void on_done_clicked(GtkButton *btn, gpointer user_data);
static void check_local_signer_availability(GnostrLogin *self);
static void save_npub_to_settings(const char *npub);
static void show_success(GnostrLogin *self, const char *npub);

static void show_toast(GnostrLogin *self, const char *msg) {
  if (!self->toast_label || !self->toast_revealer) return;
  gtk_label_set_text(GTK_LABEL(self->toast_label), msg ? msg : "");
  gtk_revealer_set_reveal_child(GTK_REVEALER(self->toast_revealer), TRUE);
  /* Auto-hide after 3 seconds */
  g_timeout_add_seconds(3, (GSourceFunc)(void*)gtk_revealer_set_reveal_child,
                        self->toast_revealer);
}

static gboolean hide_toast_on_main(gpointer user_data) {
  GtkRevealer *revealer = GTK_REVEALER(user_data);
  if (GTK_IS_REVEALER(revealer)) {
    gtk_revealer_set_reveal_child(revealer, FALSE);
  }
  return G_SOURCE_REMOVE;
}

static void gnostr_login_dispose(GObject *obj) {
  GnostrLogin *self = GNOSTR_LOGIN(obj);

  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
  }

  if (self->nip46_session) {
    nostr_nip46_session_free(self->nip46_session);
    self->nip46_session = NULL;
  }

  g_free(self->nostrconnect_uri);
  self->nostrconnect_uri = NULL;
  g_free(self->nostrconnect_secret);
  self->nostrconnect_secret = NULL;

  gtk_widget_dispose_template(GTK_WIDGET(self), GNOSTR_TYPE_LOGIN);
  G_OBJECT_CLASS(gnostr_login_parent_class)->dispose(obj);
}

static void gnostr_login_class_init(GnostrLoginClass *klass) {
  GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);
  GObjectClass *gclass = G_OBJECT_CLASS(klass);

  gclass->dispose = gnostr_login_dispose;

  gtk_widget_class_set_template_from_resource(wclass, UI_RESOURCE);

  /* Bind template children */
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, stack);
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, page_choose);
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, page_bunker);
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, page_success);

  gtk_widget_class_bind_template_child(wclass, GnostrLogin, lbl_local_status);
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, btn_local_signer);
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, spinner_local);
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, btn_remote_signer);

  gtk_widget_class_bind_template_child(wclass, GnostrLogin, qr_frame);
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, qr_picture);
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, entry_bunker_uri);
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, btn_paste_bunker);
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, btn_connect_bunker);
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, spinner_bunker);
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, lbl_bunker_status);
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, btn_back_bunker);

  gtk_widget_class_bind_template_child(wclass, GnostrLogin, lbl_success_npub);
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, btn_done);

  gtk_widget_class_bind_template_child(wclass, GnostrLogin, toast_revealer);
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, toast_label);

  /* Bind template callbacks */
  gtk_widget_class_bind_template_callback(wclass, on_local_signer_clicked);
  gtk_widget_class_bind_template_callback(wclass, on_remote_signer_clicked);
  gtk_widget_class_bind_template_callback(wclass, on_back_clicked);
  gtk_widget_class_bind_template_callback(wclass, on_paste_bunker_clicked);
  gtk_widget_class_bind_template_callback(wclass, on_connect_bunker_clicked);
  gtk_widget_class_bind_template_callback(wclass, on_close_clicked);
  gtk_widget_class_bind_template_callback(wclass, on_done_clicked);

  /**
   * GnostrLogin::signed-in:
   * @self: the login dialog
   * @npub: the bech32 npub of the signed-in user
   *
   * Emitted when the user successfully signs in.
   */
  signals[SIGNAL_SIGNED_IN] = g_signal_new(
    "signed-in",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);

  /**
   * GnostrLogin::cancelled:
   * @self: the login dialog
   *
   * Emitted when the user cancels the login.
   */
  signals[SIGNAL_CANCELLED] = g_signal_new(
    "cancelled",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 0);
}

static void gnostr_login_init(GnostrLogin *self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  self->checking_local = FALSE;
  self->connecting_local = FALSE;
  self->connecting_bunker = FALSE;
  self->local_signer_available = FALSE;
  self->cancellable = g_cancellable_new();

  /* Start checking for local signer availability */
  check_local_signer_availability(self);
}

GnostrLogin *gnostr_login_new(GtkWindow *parent) {
  GnostrLogin *self = g_object_new(GNOSTR_TYPE_LOGIN,
                                    "transient-for", parent,
                                    "modal", TRUE,
                                    NULL);
  return self;
}

/* ---- Local Signer (NIP-55L) ---- */

typedef struct {
  GnostrLogin *self;
} CheckLocalCtx;

static void check_local_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  CheckLocalCtx *ctx = (CheckLocalCtx*)user_data;
  GnostrLogin *self = ctx->self;
  g_free(ctx);

  if (!GNOSTR_IS_LOGIN(self)) return;

  self->checking_local = FALSE;
  gtk_widget_set_visible(self->spinner_local, FALSE);

  GError *error = NULL;
  char *npub = NULL;
  NostrSignerProxy *proxy = gnostr_signer_proxy_get(&error);

  if (proxy) {
    /* Try to call GetPublicKey synchronously to verify it works */
    gboolean ok = nostr_org_nostr_signer_call_get_public_key_sync(
        proxy, &npub, NULL, &error);
    if (ok && npub && *npub) {
      self->local_signer_available = TRUE;
      gtk_label_set_text(GTK_LABEL(self->lbl_local_status), "Signer available");
      gtk_widget_set_sensitive(self->btn_local_signer, TRUE);
      g_free(npub);
    } else {
      self->local_signer_available = FALSE;
      gtk_label_set_text(GTK_LABEL(self->lbl_local_status),
                         "Signer not responding");
      gtk_widget_set_sensitive(self->btn_local_signer, FALSE);
      if (error) g_error_free(error);
    }
  } else {
    self->local_signer_available = FALSE;
    gtk_label_set_text(GTK_LABEL(self->lbl_local_status),
                       "No local signer (install gnostr-signer)");
    gtk_widget_set_sensitive(self->btn_local_signer, FALSE);
    if (error) g_error_free(error);
  }
}

static gboolean check_local_idle(gpointer user_data) {
  CheckLocalCtx *ctx = (CheckLocalCtx*)user_data;
  GnostrLogin *self = ctx->self;

  if (!GNOSTR_IS_LOGIN(self)) {
    g_free(ctx);
    return G_SOURCE_REMOVE;
  }

  /* Do the D-Bus check synchronously (it's fast) then update UI */
  check_local_complete(NULL, NULL, ctx);
  return G_SOURCE_REMOVE;
}

static void check_local_signer_availability(GnostrLogin *self) {
  if (self->checking_local) return;

  self->checking_local = TRUE;
  gtk_widget_set_visible(self->spinner_local, TRUE);
  gtk_label_set_text(GTK_LABEL(self->lbl_local_status), "Checking...");

  CheckLocalCtx *ctx = g_new0(CheckLocalCtx, 1);
  ctx->self = self;
  g_idle_add(check_local_idle, ctx);
}

typedef struct {
  GnostrLogin *self;
} LocalSignInCtx;

static void local_sign_in_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  LocalSignInCtx *ctx = (LocalSignInCtx*)user_data;
  GnostrLogin *self = ctx->self;
  g_free(ctx);

  if (!GNOSTR_IS_LOGIN(self)) return;

  self->connecting_local = FALSE;
  gtk_widget_set_visible(self->spinner_local, FALSE);
  gtk_widget_set_sensitive(self->btn_local_signer, TRUE);

  GError *error = NULL;
  char *npub = NULL;
  NostrSignerProxy *proxy = gnostr_signer_proxy_get(&error);

  if (!proxy) {
    show_toast(self, "Failed to connect to local signer");
    if (error) g_error_free(error);
    return;
  }

  gboolean ok = nostr_org_nostr_signer_call_get_public_key_sync(
      proxy, &npub, NULL, &error);

  if (!ok || !npub || !*npub) {
    show_toast(self, error ? error->message : "Failed to get public key");
    if (error) g_error_free(error);
    return;
  }

  /* Success! Save to settings and show success page */
  save_npub_to_settings(npub);
  show_success(self, npub);
  g_free(npub);
}

static gboolean local_sign_in_idle(gpointer user_data) {
  LocalSignInCtx *ctx = (LocalSignInCtx*)user_data;
  local_sign_in_complete(NULL, NULL, ctx);
  return G_SOURCE_REMOVE;
}

static void on_local_signer_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrLogin *self = GNOSTR_LOGIN(user_data);
  if (!GNOSTR_IS_LOGIN(self)) return;

  if (self->connecting_local) return;

  self->connecting_local = TRUE;
  gtk_widget_set_visible(self->spinner_local, TRUE);
  gtk_widget_set_sensitive(self->btn_local_signer, FALSE);

  LocalSignInCtx *ctx = g_new0(LocalSignInCtx, 1);
  ctx->self = self;
  g_idle_add(local_sign_in_idle, ctx);
}

/* ---- Remote Signer (NIP-46) ---- */

#ifdef HAVE_QRENCODE
static GdkTexture *generate_qr_texture(const char *data) {
  QRcode *qr = QRcode_encodeString(data, 0, QR_ECLEVEL_M, QR_MODE_8, 1);
  if (!qr) return NULL;

  /* Create a pixbuf with border */
  int border = 4;
  int scale = 4;
  int size = (qr->width + border * 2) * scale;

  guchar *pixels = g_malloc(size * size * 3);
  memset(pixels, 255, size * size * 3); /* white background */

  for (int y = 0; y < qr->width; y++) {
    for (int x = 0; x < qr->width; x++) {
      if (qr->data[y * qr->width + x] & 1) {
        /* Black module */
        for (int sy = 0; sy < scale; sy++) {
          for (int sx = 0; sx < scale; sx++) {
            int px = (x + border) * scale + sx;
            int py = (y + border) * scale + sy;
            int idx = (py * size + px) * 3;
            pixels[idx] = 0;
            pixels[idx + 1] = 0;
            pixels[idx + 2] = 0;
          }
        }
      }
    }
  }

  QRcode_free(qr);

  GBytes *bytes = g_bytes_new_take(pixels, size * size * 3);
  GdkTexture *texture = gdk_memory_texture_new(size, size, GDK_MEMORY_R8G8B8, bytes, size * 3);
  g_bytes_unref(bytes);

  return texture;
}
#endif

static void generate_nostrconnect_uri(GnostrLogin *self) {
  /* Generate a random client keypair for NIP-46 */
  uint8_t secret_bytes[32];
  /* Use GLib random for now; in production use a CSPRNG */
  for (int i = 0; i < 32; i++) {
    secret_bytes[i] = g_random_int_range(0, 256);
  }

  /* Encode secret as hex */
  char secret_hex[65];
  for (int i = 0; i < 32; i++) {
    sprintf(secret_hex + i * 2, "%02x", secret_bytes[i]);
  }
  secret_hex[64] = '\0';

  g_free(self->nostrconnect_secret);
  self->nostrconnect_secret = g_strdup(secret_hex);

  /* Build nostrconnect:// URI with relay and metadata
   * Format: nostrconnect://<client-pubkey>?relay=...&secret=...&name=...
   * For now, use a placeholder since we need to compute the pubkey from secret
   */
  /* TODO: Compute actual client pubkey from secret_bytes using secp256k1 */
  const char *relay = "wss://relay.nsec.app";

  /* Create a simple nostrconnect URI */
  g_free(self->nostrconnect_uri);
  self->nostrconnect_uri = g_strdup_printf(
    "nostrconnect://%s?relay=%s&secret=%s&name=GNostr",
    "placeholder",  /* TODO: actual client pubkey */
    relay,
    secret_hex
  );

#ifdef HAVE_QRENCODE
  GdkTexture *tex = generate_qr_texture(self->nostrconnect_uri);
  if (tex && self->qr_picture) {
    gtk_picture_set_paintable(GTK_PICTURE(self->qr_picture), GDK_PAINTABLE(tex));
    g_object_unref(tex);
  }
#endif
}

static void on_remote_signer_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrLogin *self = GNOSTR_LOGIN(user_data);
  if (!GNOSTR_IS_LOGIN(self)) return;

  /* Generate nostrconnect URI for QR code */
  generate_nostrconnect_uri(self);

  /* Switch to bunker page */
  gtk_stack_set_visible_child(GTK_STACK(self->stack), self->page_bunker);
}

static void on_back_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrLogin *self = GNOSTR_LOGIN(user_data);
  if (!GNOSTR_IS_LOGIN(self)) return;

  gtk_stack_set_visible_child(GTK_STACK(self->stack), self->page_choose);
}

static void on_paste_read_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  GdkClipboard *clipboard = GDK_CLIPBOARD(source);
  GnostrLogin *self = GNOSTR_LOGIN(user_data);

  if (!GNOSTR_IS_LOGIN(self)) return;

  char *text = gdk_clipboard_read_text_finish(clipboard, res, NULL);
  if (text && *text) {
    gtk_editable_set_text(GTK_EDITABLE(self->entry_bunker_uri), text);
    g_free(text);
  }
}

static void on_paste_bunker_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrLogin *self = GNOSTR_LOGIN(user_data);
  if (!GNOSTR_IS_LOGIN(self)) return;

  GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(self));
  gdk_clipboard_read_text_async(clipboard, self->cancellable, on_paste_read_complete, self);
}

typedef struct {
  GnostrLogin *self;
  char *bunker_uri;
} BunkerConnectCtx;

static void bunker_connect_ctx_free(gpointer data) {
  BunkerConnectCtx *ctx = (BunkerConnectCtx*)data;
  if (!ctx) return;
  g_free(ctx->bunker_uri);
  g_free(ctx);
}

static void bunker_connect_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable) {
  BunkerConnectCtx *ctx = (BunkerConnectCtx*)task_data;
  (void)source;
  (void)cancellable;

  if (!ctx->bunker_uri || !*ctx->bunker_uri) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Empty bunker URI");
    return;
  }

  /* Create NIP-46 client session */
  NostrNip46Session *session = nostr_nip46_client_new();
  if (!session) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to create NIP-46 session");
    return;
  }

  /* Connect to bunker */
  int rc = nostr_nip46_client_connect(session, ctx->bunker_uri, NULL);
  if (rc != 0) {
    nostr_nip46_session_free(session);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to connect to bunker (error %d)", rc);
    return;
  }

  /* Get public key */
  char *pubkey_hex = NULL;
  rc = nostr_nip46_client_get_public_key(session, &pubkey_hex);
  if (rc != 0 || !pubkey_hex) {
    nostr_nip46_session_free(session);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to get public key from bunker");
    return;
  }

  /* Convert hex pubkey to npub */
  uint8_t pubkey_bytes[32];
  for (int i = 0; i < 32; i++) {
    unsigned int byte;
    if (sscanf(pubkey_hex + i * 2, "%2x", &byte) != 1) {
      free(pubkey_hex);
      nostr_nip46_session_free(session);
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                              "Invalid pubkey from bunker");
      return;
    }
    pubkey_bytes[i] = (uint8_t)byte;
  }
  free(pubkey_hex);

  char *npub = NULL;
  if (nostr_nip19_encode_npub(pubkey_bytes, &npub) != 0 || !npub) {
    nostr_nip46_session_free(session);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to encode npub");
    return;
  }

  /* Store session and return npub */
  char **result = g_new0(char*, 2);
  result[0] = npub; /* Transfer ownership */
  result[1] = (char*)session; /* Transfer ownership */

  g_task_return_pointer(task, result, NULL);
}

static void bunker_connect_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  GnostrLogin *self = GNOSTR_LOGIN(user_data);
  (void)source;

  if (!GNOSTR_IS_LOGIN(self)) return;

  self->connecting_bunker = FALSE;
  gtk_widget_set_visible(self->spinner_bunker, FALSE);
  gtk_widget_set_sensitive(self->btn_connect_bunker, TRUE);
  gtk_widget_set_sensitive(self->entry_bunker_uri, TRUE);

  GError *error = NULL;
  char **result = g_task_propagate_pointer(G_TASK(res), &error);

  if (error) {
    gtk_label_set_text(GTK_LABEL(self->lbl_bunker_status), error->message);
    show_toast(self, error->message);
    g_error_free(error);
    return;
  }

  if (!result || !result[0]) {
    show_toast(self, "Connection failed");
    return;
  }

  char *npub = result[0];
  NostrNip46Session *session = (NostrNip46Session*)result[1];
  g_free(result);

  /* Store session for later signing operations */
  if (self->nip46_session) {
    nostr_nip46_session_free(self->nip46_session);
  }
  self->nip46_session = session;

  /* Save npub to settings */
  save_npub_to_settings(npub);

  /* Show success */
  show_success(self, npub);
  g_free(npub);
}

static void on_connect_bunker_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrLogin *self = GNOSTR_LOGIN(user_data);
  if (!GNOSTR_IS_LOGIN(self)) return;

  if (self->connecting_bunker) return;

  const char *uri = gtk_editable_get_text(GTK_EDITABLE(self->entry_bunker_uri));
  if (!uri || !*uri) {
    show_toast(self, "Please enter a bunker:// URI");
    return;
  }

  /* Validate URI starts with bunker:// or nostrconnect:// */
  if (!g_str_has_prefix(uri, "bunker://") && !g_str_has_prefix(uri, "nostrconnect://")) {
    show_toast(self, "Invalid URI format (expected bunker:// or nostrconnect://)");
    return;
  }

  self->connecting_bunker = TRUE;
  gtk_widget_set_visible(self->spinner_bunker, TRUE);
  gtk_widget_set_sensitive(self->btn_connect_bunker, FALSE);
  gtk_widget_set_sensitive(self->entry_bunker_uri, FALSE);
  gtk_label_set_text(GTK_LABEL(self->lbl_bunker_status), "Connecting...");

  BunkerConnectCtx *ctx = g_new0(BunkerConnectCtx, 1);
  ctx->self = self;
  ctx->bunker_uri = g_strdup(uri);

  GTask *task = g_task_new(NULL, self->cancellable, bunker_connect_complete, self);
  g_task_set_task_data(task, ctx, bunker_connect_ctx_free);
  g_task_run_in_thread(task, bunker_connect_thread);
  g_object_unref(task);
}

/* ---- Utilities ---- */

static void save_npub_to_settings(const char *npub) {
  GSettings *settings = g_settings_new(SETTINGS_SCHEMA_CLIENT);
  if (settings) {
    g_settings_set_string(settings, SETTINGS_KEY_CURRENT_NPUB, npub ? npub : "");
    g_object_unref(settings);
  }
}

static void show_success(GnostrLogin *self, const char *npub) {
  if (self->lbl_success_npub) {
    gtk_label_set_text(GTK_LABEL(self->lbl_success_npub), npub ? npub : "");
  }

  gtk_stack_set_visible_child(GTK_STACK(self->stack), self->page_success);

  /* Emit signed-in signal */
  g_signal_emit(self, signals[SIGNAL_SIGNED_IN], 0, npub);
}

static void on_close_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrLogin *self = GNOSTR_LOGIN(user_data);
  if (!GNOSTR_IS_LOGIN(self)) return;

  g_signal_emit(self, signals[SIGNAL_CANCELLED], 0);
  gtk_window_close(GTK_WINDOW(self));
}

static void on_done_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrLogin *self = GNOSTR_LOGIN(user_data);
  if (!GNOSTR_IS_LOGIN(self)) return;

  gtk_window_close(GTK_WINDOW(self));
}
