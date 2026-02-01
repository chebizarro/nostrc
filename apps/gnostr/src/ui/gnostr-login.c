/**
 * GnostrLogin - Login Dialog for NIP-55L and NIP-46 Authentication
 *
 * Provides sign-in options:
 * 1. NIP-55L: Local signer via D-Bus (gnostr-signer)
 * 2. NIP-46: Remote signer via bunker:// URI with QR code display
 */

#include "gnostr-login.h"
#include <adwaita.h>
#include "../ipc/signer_ipc.h"
#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_uri.h"
#include "nostr/nip19/nip19.h"
#include "nostr/nip44/nip44.h"
#include "nostr_simple_pool.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-kinds.h"
#include <glib/gi18n.h>
#include <json.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>

/* QR code generation - using qrencode if available */
#ifdef HAVE_QRENCODE
#include <qrencode.h>
#endif

#define UI_RESOURCE "/org/gnostr/ui/ui/dialogs/gnostr-login.ui"

/* Settings schema and key names */
#define SETTINGS_SCHEMA_CLIENT "org.gnostr.Client"
#define SETTINGS_KEY_CURRENT_NPUB "current-npub"

struct _GnostrLogin {
  AdwBin parent_instance;

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
  GtkWidget *btn_cancel_bunker;
  GtkWidget *btn_retry_bunker;
  GtkWidget *btn_back_bunker;

  /* Status area widgets */
  GtkWidget *status_frame;
  GtkWidget *status_icon_stack;
  GtkWidget *spinner_bunker;
  GtkWidget *status_icon_success;
  GtkWidget *status_icon_error;
  GtkWidget *status_icon_waiting;
  GtkWidget *lbl_bunker_status;
  GtkWidget *lbl_bunker_status_detail;

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
  char *nostrconnect_secret;       /* Secret for bunker auth (hex) */
  uint8_t nostrconnect_secret_bytes[32]; /* Secret bytes for decryption */
  char *client_pubkey_hex;         /* Client pubkey from nostrconnect URI */
  NostrNip46Session *nip46_session; /* NIP-46 session */
  GCancellable *cancellable;       /* For async operations */

  /* NIP-46 relay subscription for receiving signer responses */
  GnostrSimplePool *nip46_pool;
  gulong nip46_events_handler;
  gboolean listening_for_response;
};

G_DEFINE_TYPE(GnostrLogin, gnostr_login, ADW_TYPE_BIN)

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
static void on_cancel_bunker_clicked(GtkButton *btn, gpointer user_data);
static void on_retry_bunker_clicked(GtkButton *btn, gpointer user_data);
static void on_close_clicked(GtkButton *btn, gpointer user_data);
static void on_done_clicked(GtkButton *btn, gpointer user_data);
static void check_local_signer_availability(GnostrLogin *self);
static void save_npub_to_settings(const char *npub);
static void save_nip46_credentials_to_settings(const char *client_secret_hex,
                                                const char *signer_pubkey_hex,
                                                const char *relay_url);
static void show_success(GnostrLogin *self, const char *npub);
static void start_nip46_listener(GnostrLogin *self, const char *relay_url);
static void stop_nip46_listener(GnostrLogin *self);
static void on_nip46_events(GnostrSimplePool *pool, GPtrArray *batch, gpointer user_data);

/* Status state enum for bunker connection */
typedef enum {
  BUNKER_STATUS_IDLE,
  BUNKER_STATUS_CONNECTING,
  BUNKER_STATUS_WAITING,
  BUNKER_STATUS_SUCCESS,
  BUNKER_STATUS_ERROR
} BunkerStatusState;

static void set_bunker_status(GnostrLogin *self, BunkerStatusState state,
                               const char *message, const char *detail);

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

/* Helper to set bunker status UI state */
static void set_bunker_status(GnostrLogin *self, BunkerStatusState state,
                               const char *message, const char *detail) {
  if (!GNOSTR_IS_LOGIN(self)) return;

  /* Show/hide status frame */
  gboolean show_status = (state != BUNKER_STATUS_IDLE);
  if (self->status_frame) {
    gtk_widget_set_visible(self->status_frame, show_status);
  }

  /* Set status message */
  if (self->lbl_bunker_status) {
    gtk_label_set_text(GTK_LABEL(self->lbl_bunker_status), message ? message : "");
  }

  /* Set detail message */
  if (self->lbl_bunker_status_detail) {
    gtk_label_set_text(GTK_LABEL(self->lbl_bunker_status_detail), detail ? detail : "");
    gtk_widget_set_visible(self->lbl_bunker_status_detail, detail && *detail);
  }

  /* Set icon based on state */
  const char *icon_child = "spinner_bunker";
  switch (state) {
    case BUNKER_STATUS_IDLE:
      icon_child = "spinner_bunker";
      break;
    case BUNKER_STATUS_CONNECTING:
      icon_child = "spinner_bunker";
      if (self->spinner_bunker) {
        gtk_spinner_start(GTK_SPINNER(self->spinner_bunker));
      }
      break;
    case BUNKER_STATUS_WAITING:
      icon_child = "status_icon_waiting";
      break;
    case BUNKER_STATUS_SUCCESS:
      icon_child = "status_icon_success";
      break;
    case BUNKER_STATUS_ERROR:
      icon_child = "status_icon_error";
      break;
  }

  if (self->status_icon_stack) {
    GtkWidget *child = gtk_stack_get_child_by_name(GTK_STACK(self->status_icon_stack), icon_child);
    if (child) {
      gtk_stack_set_visible_child(GTK_STACK(self->status_icon_stack), child);
    }
  }

  /* Update button visibility */
  gboolean is_connecting = (state == BUNKER_STATUS_CONNECTING || state == BUNKER_STATUS_WAITING);
  gboolean is_error = (state == BUNKER_STATUS_ERROR);

  if (self->btn_connect_bunker) {
    gtk_widget_set_visible(self->btn_connect_bunker, !is_connecting && !is_error);
  }
  if (self->btn_cancel_bunker) {
    gtk_widget_set_visible(self->btn_cancel_bunker, is_connecting);
  }
  if (self->btn_retry_bunker) {
    gtk_widget_set_visible(self->btn_retry_bunker, is_error);
  }

  /* Enable/disable input during connection */
  if (self->entry_bunker_uri) {
    gtk_widget_set_sensitive(self->entry_bunker_uri, !is_connecting);
  }
  if (self->btn_paste_bunker) {
    gtk_widget_set_sensitive(self->btn_paste_bunker, !is_connecting);
  }
  if (self->btn_back_bunker) {
    gtk_widget_set_sensitive(self->btn_back_bunker, !is_connecting);
  }
}

/* Deferred NIP-46 success context - used to safely stop the listener
 * and update UI AFTER the event callback completes */
typedef struct {
  GnostrLogin *self;
  char *npub;
  char *signer_pubkey_hex;  /* nostrc-rrfr: signer pubkey for session */
  char *nostrconnect_uri;   /* nostrc-rrfr: URI for session secret/relays */
  char *nostrconnect_secret; /* nostrc-1wfi: client private key for GSettings persistence */
} Nip46SuccessCtx;

static void nip46_success_ctx_free(gpointer data) {
  Nip46SuccessCtx *ctx = data;
  if (ctx) {
    g_clear_object(&ctx->self);
    g_free(ctx->npub);
    g_free(ctx->signer_pubkey_hex);
    g_free(ctx->nostrconnect_uri);
    /* nostrc-1wfi: Securely clear the secret before freeing */
    if (ctx->nostrconnect_secret) {
      memset(ctx->nostrconnect_secret, 0, strlen(ctx->nostrconnect_secret));
      g_free(ctx->nostrconnect_secret);
    }
    g_free(ctx);
  }
}

static gboolean nip46_success_on_main(gpointer data) {
  Nip46SuccessCtx *ctx = data;
  if (!ctx || !ctx->self || !GNOSTR_IS_LOGIN(ctx->self)) {
    return G_SOURCE_REMOVE;
  }

  GnostrLogin *self = ctx->self;

  /* Now safe to stop the listener (we're not in the callback anymore) */
  stop_nip46_listener(self);

  /* Create NIP-46 session for future signing operations */
  if (self->nip46_session) {
    nostr_nip46_session_free(self->nip46_session);
  }
  self->nip46_session = nostr_nip46_client_new();

  /* nostrc-rrfr: Populate session with connection info from the nostrconnect URI */
  if (ctx->nostrconnect_uri) {
    g_message("[NIP46_LOGIN] Populating session from URI: %.80s...", ctx->nostrconnect_uri);
    int rc = nostr_nip46_client_connect(self->nip46_session, ctx->nostrconnect_uri, NULL);
    if (rc != 0) {
      g_warning("[NIP46_LOGIN] Failed to populate session from URI: %d", rc);
    } else {
      g_message("[NIP46_LOGIN] Session populated with secret and relays from URI");
    }
  } else {
    g_warning("[NIP46_LOGIN] nostrconnect_uri is NULL - session won't have secret key!");
  }

  /* nostrc-rrfr: Store the signer's pubkey in the session - critical for signing */
  if (ctx->signer_pubkey_hex) {
    if (nostr_nip46_client_set_signer_pubkey(self->nip46_session, ctx->signer_pubkey_hex) != 0) {
      g_warning("[NIP46_LOGIN] Failed to set signer pubkey in session");
    } else {
      g_message("[NIP46_LOGIN] Signer pubkey stored in session: %s", ctx->signer_pubkey_hex);
    }
  }

  /* nostrc-1wfi: Persist NIP-46 credentials to GSettings for app restart survival */
  if (ctx->nostrconnect_secret && ctx->signer_pubkey_hex) {
    const char *relay_url = "wss://relay.nsec.app"; /* Default NIP-46 relay */
    save_nip46_credentials_to_settings(ctx->nostrconnect_secret,
                                        ctx->signer_pubkey_hex,
                                        relay_url);
  } else {
    g_warning("[NIP46_LOGIN] Cannot persist credentials: secret=%s, pubkey=%s",
              ctx->nostrconnect_secret ? "set" : "NULL",
              ctx->signer_pubkey_hex ? "set" : "NULL");
  }

  /* Save to settings and show success */
  save_npub_to_settings(ctx->npub);
  show_success(self, ctx->npub);

  return G_SOURCE_REMOVE;
}

static void gnostr_login_dispose(GObject *obj) {
  GnostrLogin *self = GNOSTR_LOGIN(obj);

  /* Stop NIP-46 relay listener */
  stop_nip46_listener(self);

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
  g_free(self->client_pubkey_hex);
  self->client_pubkey_hex = NULL;

  /* Clear secret bytes from memory */
  memset(self->nostrconnect_secret_bytes, 0, sizeof(self->nostrconnect_secret_bytes));

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
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, btn_cancel_bunker);
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, btn_retry_bunker);
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, btn_back_bunker);
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, status_frame);
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, status_icon_stack);
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, spinner_bunker);
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, status_icon_success);
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, status_icon_error);
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, status_icon_waiting);
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, lbl_bunker_status);
  gtk_widget_class_bind_template_child(wclass, GnostrLogin, lbl_bunker_status_detail);

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
  gtk_widget_class_bind_template_callback(wclass, on_cancel_bunker_clicked);
  gtk_widget_class_bind_template_callback(wclass, on_retry_bunker_clicked);
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

GnostrLogin *gnostr_login_new(void) {
  return g_object_new(GNOSTR_TYPE_LOGIN, NULL);
}

NostrNip46Session *gnostr_login_take_nip46_session(GnostrLogin *self) {
  g_return_val_if_fail(GNOSTR_IS_LOGIN(self), NULL);

  NostrNip46Session *session = self->nip46_session;
  self->nip46_session = NULL;
  return session;
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

  /* Store secret bytes for NIP-44 decryption later */
  memcpy(self->nostrconnect_secret_bytes, secret_bytes, 32);

  /* Encode secret as hex for the URI query parameter */
  char secret_hex[65];
  for (int i = 0; i < 32; i++) {
    snprintf(secret_hex + i * 2, 3, "%02x", secret_bytes[i]);
  }
  secret_hex[64] = '\0';

  g_free(self->nostrconnect_secret);
  self->nostrconnect_secret = g_strdup(secret_hex);

  /* Compute client pubkey from secret_bytes using secp256k1 */
  char client_pubkey_hex[65] = {0};
  secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
  if (ctx) {
    secp256k1_keypair keypair;
    if (secp256k1_keypair_create(ctx, &keypair, secret_bytes)) {
      secp256k1_xonly_pubkey xonly_pubkey;
      if (secp256k1_keypair_xonly_pub(ctx, &xonly_pubkey, NULL, &keypair)) {
        uint8_t pubkey_bytes[32];
        secp256k1_xonly_pubkey_serialize(ctx, pubkey_bytes, &xonly_pubkey);
        /* Encode pubkey as hex */
        for (int i = 0; i < 32; i++) {
          snprintf(client_pubkey_hex + i * 2, 3, "%02x", pubkey_bytes[i]);
        }
        client_pubkey_hex[64] = '\0';
      }
    }
    secp256k1_context_destroy(ctx);
  }

  /* Fallback if pubkey derivation failed */
  if (client_pubkey_hex[0] == '\0') {
    g_warning("Failed to derive client pubkey from secret");
    /* Clear secret bytes on failure */
    memset(secret_bytes, 0, sizeof(secret_bytes));
    memset(self->nostrconnect_secret_bytes, 0, sizeof(self->nostrconnect_secret_bytes));
    return;
  }

  /* Store client pubkey for subscription filter */
  g_free(self->client_pubkey_hex);
  self->client_pubkey_hex = g_strdup(client_pubkey_hex);

  /* Clear secret bytes from stack (we've stored them in self) */
  memset(secret_bytes, 0, sizeof(secret_bytes));

  /* Build nostrconnect:// URI with relay and metadata
   * Format: nostrconnect://<client-pubkey>?relay=...&secret=...&name=...
   */
  const char *relay = "wss://relay.nsec.app";

  /* Create the nostrconnect URI with the computed client pubkey */
  g_free(self->nostrconnect_uri);
  self->nostrconnect_uri = g_strdup_printf(
    "nostrconnect://%s?relay=%s&secret=%s&name=GNostr",
    client_pubkey_hex,
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

  /* Start listening for NIP-46 responses on the relay */
  const char *relay = "wss://relay.nsec.app";
  start_nip46_listener(self, relay);

  /* Update status to waiting state */
  set_bunker_status(self, BUNKER_STATUS_WAITING,
                    "Waiting for approval...",
                    "Scan the QR code with your signer app (Amber, nsec.app, etc.)");
}

static void on_back_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrLogin *self = GNOSTR_LOGIN(user_data);
  if (!GNOSTR_IS_LOGIN(self)) return;

  /* Stop listening for NIP-46 responses */
  stop_nip46_listener(self);

  /* Reset status */
  set_bunker_status(self, BUNKER_STATUS_IDLE, "", NULL);

  gtk_stack_set_visible_child(GTK_STACK(self->stack), self->page_choose);
}

static void on_cancel_bunker_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrLogin *self = GNOSTR_LOGIN(user_data);
  if (!GNOSTR_IS_LOGIN(self)) return;

  /* Cancel any pending operations */
  if (self->cancellable) {
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
    self->cancellable = g_cancellable_new();
  }

  /* Stop listening for NIP-46 responses */
  stop_nip46_listener(self);

  self->connecting_bunker = FALSE;

  /* Reset status to idle */
  set_bunker_status(self, BUNKER_STATUS_IDLE, "", NULL);
}

static void on_retry_bunker_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrLogin *self = GNOSTR_LOGIN(user_data);
  if (!GNOSTR_IS_LOGIN(self)) return;

  /* Reset to idle state, then user can try again */
  set_bunker_status(self, BUNKER_STATUS_IDLE, "", NULL);

  /* Re-generate nostrconnect URI and restart listener */
  generate_nostrconnect_uri(self);

  const char *relay = "wss://relay.nsec.app";
  start_nip46_listener(self, relay);

  /* Set waiting status */
  set_bunker_status(self, BUNKER_STATUS_WAITING,
                    "Waiting for approval...",
                    "Scan the QR code with your signer app");
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

  GError *error = NULL;
  char **result = g_task_propagate_pointer(G_TASK(res), &error);

  if (error) {
    /* Show error state with helpful message */
    const char *detail = NULL;
    if (g_str_has_prefix(error->message, "Failed to connect")) {
      detail = "Check the URI is correct and the signer is online";
    } else if (g_str_has_prefix(error->message, "Failed to get public key")) {
      detail = "The signer did not return your public key";
    }
    set_bunker_status(self, BUNKER_STATUS_ERROR, error->message, detail);
    g_error_free(error);
    return;
  }

  if (!result || !result[0]) {
    set_bunker_status(self, BUNKER_STATUS_ERROR,
                      "Connection failed",
                      "The signer did not respond. Try again.");
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
    show_toast(self, "Please enter a connection URI");
    return;
  }

  /* Validate URI starts with bunker:// or nostrconnect:// */
  if (!g_str_has_prefix(uri, "bunker://") && !g_str_has_prefix(uri, "nostrconnect://")) {
    set_bunker_status(self, BUNKER_STATUS_ERROR,
                      "Invalid URI format",
                      "URI must start with bunker:// or nostrconnect://");
    return;
  }

  self->connecting_bunker = TRUE;

  /* Show connecting status */
  set_bunker_status(self, BUNKER_STATUS_CONNECTING,
                    "Connecting to signer...",
                    "Establishing secure connection");

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

/* nostrc-1wfi: Save NIP-46 credentials for session persistence across app restarts */
static void save_nip46_credentials_to_settings(const char *client_secret_hex,
                                                const char *signer_pubkey_hex,
                                                const char *relay_url) {
  GSettings *settings = g_settings_new(SETTINGS_SCHEMA_CLIENT);
  if (!settings) return;

  g_settings_set_string(settings, "nip46-client-secret",
                        client_secret_hex ? client_secret_hex : "");
  g_settings_set_string(settings, "nip46-signer-pubkey",
                        signer_pubkey_hex ? signer_pubkey_hex : "");
  g_settings_set_string(settings, "nip46-relay",
                        relay_url ? relay_url : "");

  g_message("[NIP46_LOGIN] Saved NIP-46 credentials to settings (secret: %zu chars, pubkey: %s)",
            client_secret_hex ? strlen(client_secret_hex) : 0,
            signer_pubkey_hex ? signer_pubkey_hex : "(null)");

  g_object_unref(settings);
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

/* ---- NIP-46 Relay Listener for Remote Signer Responses ---- */

/* NIP-46 response kind */
#define NIP46_RESPONSE_KIND 24133

static void on_nip46_subscribe_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  GnostrLogin *self = GNOSTR_LOGIN(user_data);
  GError *error = NULL;

  gnostr_simple_pool_subscribe_many_finish(GNOSTR_SIMPLE_POOL(source), res, &error);

  if (error) {
    g_warning("[NIP46_LOGIN] Subscription failed: %s", error->message);
    if (GNOSTR_IS_LOGIN(self)) {
      set_bunker_status(self, BUNKER_STATUS_ERROR,
                        "Failed to connect to relay",
                        "Check your internet connection and try again");
    }
    g_clear_error(&error);
  } else {
    g_message("[NIP46_LOGIN] Listening for signer response...");
  }

  if (self) g_object_unref(self);
}

/* Handle incoming NIP-46 events from the relay */
static void on_nip46_events(GnostrSimplePool *pool, GPtrArray *batch, gpointer user_data) {
  (void)pool;
  GnostrLogin *self = GNOSTR_LOGIN(user_data);

  if (!GNOSTR_IS_LOGIN(self) || !batch) return;

  for (guint i = 0; i < batch->len; i++) {
    NostrEvent *event = g_ptr_array_index(batch, i);
    if (!event) continue;

    int kind = nostr_event_get_kind(event);
    if (kind != NIP46_RESPONSE_KIND) continue;

    const char *content = nostr_event_get_content(event);
    const char *sender_pubkey = nostr_event_get_pubkey(event);

    if (!content || !sender_pubkey) continue;

    g_message("[NIP46_LOGIN] Received NIP-46 event from %s", sender_pubkey);

    /* Decrypt content using NIP-44 with our client secret and the sender's pubkey */
    uint8_t sender_pubkey_bytes[32];
    for (int j = 0; j < 32; j++) {
      unsigned int byte;
      if (sscanf(sender_pubkey + j * 2, "%2x", &byte) != 1) {
        g_warning("[NIP46_LOGIN] Invalid sender pubkey");
        continue;
      }
      sender_pubkey_bytes[j] = (uint8_t)byte;
    }

    /* Decrypt the content using NIP-44 v2 */
    uint8_t *plaintext_bytes = NULL;
    size_t plaintext_len = 0;
    int rc = nostr_nip44_decrypt_v2(
      self->nostrconnect_secret_bytes,
      sender_pubkey_bytes,
      content,
      &plaintext_bytes,
      &plaintext_len
    );

    if (rc != 0 || !plaintext_bytes) {
      g_warning("[NIP46_LOGIN] Failed to decrypt NIP-46 response: %d", rc);
      continue;
    }

    /* Null-terminate the plaintext for JSON parsing */
    char *plaintext = g_strndup((const char*)plaintext_bytes, plaintext_len);
    free(plaintext_bytes);

    g_message("[NIP46_LOGIN] Decrypted response: %s", plaintext);

    /* Parse the NIP-46 response JSON:
     * {"id":"...","result":"<signer_pubkey>","error":null}
     * For connect request, result contains the signer's pubkey
     */
    if (!nostr_json_is_valid(plaintext)) {
      g_warning("[NIP46_LOGIN] Failed to parse NIP-46 JSON");
      g_free(plaintext);
      continue;
    }

    /* Check for error - if key exists and is a non-null string */
    char *err_msg = NULL;
    if (nostr_json_has_key(plaintext, "error") &&
        nostr_json_get_type(plaintext, "error") == NOSTR_JSON_STRING &&
        nostr_json_get_string(plaintext, "error", &err_msg) == 0 && err_msg && *err_msg) {
      g_warning("[NIP46_LOGIN] Signer error: %s", err_msg);
      set_bunker_status(self, BUNKER_STATUS_ERROR, err_msg,
                        "The remote signer rejected the request");
      free(err_msg);
      g_free(plaintext);
      continue;
    }
    free(err_msg);

    /* Get the result (signer's pubkey for connect, or "ack" for simple response) */
    char *result = NULL;
    if (nostr_json_get_string(plaintext, "result", &result) != 0 || !result) {
      g_warning("[NIP46_LOGIN] No result in NIP-46 response");
      free(result);
      g_free(plaintext);
      continue;
    }

    g_message("[NIP46_LOGIN] Authorization received, signer pubkey: %s", result);

    /* For nostrconnect, the result is "ack" and we use the sender's pubkey as the signer */
    char *signer_pubkey_hex = NULL;
    if (strcmp(result, "ack") == 0) {
      signer_pubkey_hex = g_strdup(sender_pubkey);
    } else if (strlen(result) == 64) {
      /* Result is the signer's pubkey */
      signer_pubkey_hex = g_strdup(result);
    } else {
      g_warning("[NIP46_LOGIN] Unexpected result format: %s", result);
      free(result);
      g_free(plaintext);
      continue;
    }
    free(result);
    g_free(plaintext);

    /* Convert hex pubkey to npub */
    uint8_t pubkey_bytes[32];
    for (int j = 0; j < 32; j++) {
      unsigned int byte;
      if (sscanf(signer_pubkey_hex + j * 2, "%2x", &byte) != 1) {
        g_warning("[NIP46_LOGIN] Invalid signer pubkey");
        g_free(signer_pubkey_hex);
        continue;
      }
      pubkey_bytes[j] = (uint8_t)byte;
    }

    char *npub = NULL;
    if (nostr_nip19_encode_npub(pubkey_bytes, &npub) != 0 || !npub) {
      g_warning("[NIP46_LOGIN] Failed to encode npub");
      g_free(signer_pubkey_hex);
      continue;
    }

    /* Defer the stop/success operations to after this callback completes.
     * Stopping the listener from within the signal callback can cause issues
     * since we're modifying the signal handler list during emission. */
    Nip46SuccessCtx *ctx = g_new0(Nip46SuccessCtx, 1);
    ctx->self = g_object_ref(self);
    ctx->npub = g_strdup(npub);
    /* nostrc-rrfr: Pass signer info to deferred handler for session population */
    ctx->signer_pubkey_hex = g_strdup(signer_pubkey_hex);
    ctx->nostrconnect_uri = g_strdup(self->nostrconnect_uri);
    /* nostrc-1wfi: Copy secret to context so it survives until idle callback */
    ctx->nostrconnect_secret = g_strdup(self->nostrconnect_secret);

    g_free(signer_pubkey_hex);
    free(npub);

    g_idle_add_full(G_PRIORITY_HIGH, nip46_success_on_main, ctx, nip46_success_ctx_free);
    return;
  }
}

static void start_nip46_listener(GnostrLogin *self, const char *relay_url) {
  if (!self || !relay_url) return;

  if (self->listening_for_response) {
    g_warning("[NIP46_LOGIN] Already listening for response");
    return;
  }

  if (!self->client_pubkey_hex) {
    g_warning("[NIP46_LOGIN] No client pubkey set");
    return;
  }

  g_message("[NIP46_LOGIN] Starting listener on %s for pubkey %s",
            relay_url, self->client_pubkey_hex);

  /* Create pool */
  self->nip46_pool = gnostr_simple_pool_new();
  if (!self->nip46_pool) {
    g_warning("[NIP46_LOGIN] Failed to create pool");
    return;
  }

  /* Build filter for NIP-46 responses addressed to our client pubkey */
  NostrFilter *filter = nostr_filter_new();
  int kinds[] = { NIP46_RESPONSE_KIND };
  nostr_filter_set_kinds(filter, kinds, 1);

  /* Filter by p-tag for our client pubkey */
  nostr_filter_tags_append(filter, "p", self->client_pubkey_hex, NULL);

  NostrFilters *filters = nostr_filters_new();
  nostr_filters_add(filters, filter);

  /* Connect events signal */
  self->nip46_events_handler = g_signal_connect(
    self->nip46_pool, "events",
    G_CALLBACK(on_nip46_events), self);

  /* Start subscription */
  const char *relays[] = { relay_url, NULL };
  gnostr_simple_pool_subscribe_many_async(
    self->nip46_pool,
    relays,
    1,
    filters,
    self->cancellable,
    on_nip46_subscribe_done,
    g_object_ref(self));

  nostr_filters_free(filters);

  self->listening_for_response = TRUE;
}

static void stop_nip46_listener(GnostrLogin *self) {
  if (!self) return;

  if (!self->listening_for_response) return;

  g_message("[NIP46_LOGIN] Stopping listener");

  if (self->nip46_pool) {
    if (self->nip46_events_handler > 0) {
      g_signal_handler_disconnect(self->nip46_pool, self->nip46_events_handler);
      self->nip46_events_handler = 0;
    }
    g_clear_object(&self->nip46_pool);
  }

  self->listening_for_response = FALSE;
  gtk_widget_set_visible(self->spinner_bunker, FALSE);
}
