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
#include "nostr-keys.h"
#include <glib/gi18n.h>
#include <json.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>

/* QR code generation - using qrencode if available */
#ifdef HAVE_QRENCODE
#include <qrencode.h>
#endif

#define UI_RESOURCE "/org/gnostr/ui/ui/dialogs/gnostr-login.ui"

/* Default NIP-46 relay used when generating nostrconnect:// URIs for QR codes.
 * This is the relay the client will listen on for signer responses. */
#define NIP46_DEFAULT_RELAY "wss://relay.nsec.app"

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
  gulong name_owner_handler;         /* Monitor daemon appearing/disappearing */
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
static void on_name_owner_changed(GObject *proxy, GParamSpec *pspec, gpointer user_data);
static void save_npub_to_settings(const char *npub);
static void save_nip46_credentials_to_settings(const char *client_secret_hex,
                                                const char *signer_pubkey_hex,
                                                char **relays,
                                                size_t n_relays);
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

  /* Set icon based on state - use widget pointers directly since
   * GtkStack children aren't wrapped in named StackPages */
  GtkWidget *icon_widget = NULL;
  switch (state) {
    case BUNKER_STATUS_IDLE:
    case BUNKER_STATUS_CONNECTING:
      icon_widget = self->spinner_bunker;
      if (self->spinner_bunker) {
        gtk_spinner_start(GTK_SPINNER(self->spinner_bunker));
      }
      break;
    case BUNKER_STATUS_WAITING:
      icon_widget = self->status_icon_waiting;
      break;
    case BUNKER_STATUS_SUCCESS:
      icon_widget = self->status_icon_success;
      break;
    case BUNKER_STATUS_ERROR:
      icon_widget = self->status_icon_error;
      break;
  }

  if (self->status_icon_stack && icon_widget) {
    gtk_stack_set_visible_child(GTK_STACK(self->status_icon_stack), icon_widget);
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

/* Deferred NIP-46 connect success context - used to safely stop the listener
 * and call get_public_key RPC AFTER the event callback completes */
typedef struct {
  GnostrLogin *self;
  char *signer_pubkey_hex;   /* nostrc-rrfr: signer communication pubkey (sender of connect) */
  char *nostrconnect_uri;    /* nostrc-rrfr: URI for session secret/relays */
  char *nostrconnect_secret; /* nostrc-1wfi: client private key for ECDH */
  char *relay_url;           /* Relay URL for get_public_key RPC */
} Nip46ConnectCtx;

/* Context for get_public_key RPC result - passed to main thread */
typedef struct {
  GnostrLogin *self;
  char *user_pubkey_hex;     /* Actual user pubkey from get_public_key RPC */
  char *signer_pubkey_hex;   /* Signer's communication pubkey */
  char *nostrconnect_uri;    /* URI for session persistence */
  char *nostrconnect_secret; /* Client private key for persistence */
} Nip46PubkeyCtx;

static void nip46_connect_ctx_free(gpointer data) {
  Nip46ConnectCtx *ctx = data;
  if (ctx) {
    g_clear_object(&ctx->self);
    g_free(ctx->signer_pubkey_hex);
    g_free(ctx->nostrconnect_uri);
    g_free(ctx->relay_url);
    /* Securely clear the secret before freeing */
    if (ctx->nostrconnect_secret) {
      memset(ctx->nostrconnect_secret, 0, strlen(ctx->nostrconnect_secret));
      g_free(ctx->nostrconnect_secret);
    }
    g_free(ctx);
  }
}

static void nip46_pubkey_ctx_free(gpointer data) {
  Nip46PubkeyCtx *ctx = data;
  if (ctx) {
    g_clear_object(&ctx->self);
    g_free(ctx->user_pubkey_hex);
    g_free(ctx->signer_pubkey_hex);
    g_free(ctx->nostrconnect_uri);
    if (ctx->nostrconnect_secret) {
      memset(ctx->nostrconnect_secret, 0, strlen(ctx->nostrconnect_secret));
      g_free(ctx->nostrconnect_secret);
    }
    g_free(ctx);
  }
}

/* Final success handler - called after get_public_key RPC returns */
static gboolean on_nip46_pubkey_result(gpointer data) {
  Nip46PubkeyCtx *ctx = data;
  if (!ctx || !ctx->self || !GNOSTR_IS_LOGIN(ctx->self)) {
    g_warning("[NIP46_LOGIN] on_nip46_pubkey_result: invalid context");
    return G_SOURCE_REMOVE;
  }

  GnostrLogin *self = ctx->self;

  g_message("[NIP46_LOGIN] *** FINAL PUBKEY RESULT ***");
  g_message("[NIP46_LOGIN] User pubkey from get_public_key RPC: %s", ctx->user_pubkey_hex);
  g_message("[NIP46_LOGIN] Signer pubkey (for communication): %s", ctx->signer_pubkey_hex);

  /* Convert hex pubkey to npub */
  uint8_t pubkey_bytes[32];
  for (int j = 0; j < 32; j++) {
    unsigned int byte;
    if (sscanf(ctx->user_pubkey_hex + j * 2, "%2x", &byte) != 1) {
      g_warning("[NIP46_LOGIN] Invalid user pubkey from RPC");
      set_bunker_status(self, BUNKER_STATUS_ERROR,
                        "Invalid pubkey received",
                        "The signer returned an invalid public key");
      return G_SOURCE_REMOVE;
    }
    pubkey_bytes[j] = (uint8_t)byte;
  }

  char *npub = NULL;
  if (nostr_nip19_encode_npub(pubkey_bytes, &npub) != 0 || !npub) {
    g_warning("[NIP46_LOGIN] Failed to encode npub");
    set_bunker_status(self, BUNKER_STATUS_ERROR,
                      "Failed to encode npub",
                      NULL);
    return G_SOURCE_REMOVE;
  }

  /* Persist NIP-46 credentials to GSettings */
  if (ctx->nostrconnect_secret && ctx->signer_pubkey_hex) {
    char **relays = NULL;
    size_t n_relays = 0;

    if (self->nip46_session) {
      nostr_nip46_session_get_relays(self->nip46_session, &relays, &n_relays);
    }

    save_nip46_credentials_to_settings(ctx->nostrconnect_secret,
                                        ctx->signer_pubkey_hex,
                                        relays, n_relays);

    if (relays) {
      for (size_t i = 0; i < n_relays; i++) free(relays[i]);
      free(relays);
    }
  }

  /* Save and show success with the ACTUAL user pubkey */
  save_npub_to_settings(npub);
  show_success(self, npub);
  free(npub);

  return G_SOURCE_REMOVE;
}

/* GTask thread function for get_public_key RPC */
static void nip46_get_pubkey_thread(GTask *task,
                                    gpointer source_object,
                                    gpointer task_data,
                                    GCancellable *cancellable) {
  (void)source_object;
  (void)cancellable;
  Nip46ConnectCtx *ctx = task_data;

  g_message("[NIP46_LOGIN] *** STARTING get_public_key RPC THREAD ***");
  g_message("[NIP46_LOGIN] Context: signer_pubkey=%s", ctx->signer_pubkey_hex);
  g_message("[NIP46_LOGIN] Context: nostrconnect_uri=%.60s...", ctx->nostrconnect_uri ? ctx->nostrconnect_uri : "(null)");
  g_message("[NIP46_LOGIN] Context: nostrconnect_secret=%s", ctx->nostrconnect_secret ? "present" : "NULL");

  /* Create a fresh session for the RPC call */
  NostrNip46Session *rpc_session = nostr_nip46_client_new();
  if (!rpc_session) {
    g_warning("[NIP46_LOGIN] Failed to create RPC session");
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to create RPC session");
    return;
  }

  /* Set up the session for RPC:
   * - remote_pubkey = signer's communication pubkey
   * - secret = our client private key
   * - relays = from URI */
  if (ctx->nostrconnect_uri) {
    nostr_nip46_client_connect(rpc_session, ctx->nostrconnect_uri, NULL);
  }
  if (ctx->nostrconnect_secret) {
    nostr_nip46_client_set_secret(rpc_session, ctx->nostrconnect_secret);
  }
  if (ctx->signer_pubkey_hex) {
    nostr_nip46_client_set_signer_pubkey(rpc_session, ctx->signer_pubkey_hex);
  }

  /* Call get_public_key RPC to get the user's actual pubkey */
  char *user_pubkey_hex = NULL;
  int rc = nostr_nip46_client_get_public_key_rpc(rpc_session, &user_pubkey_hex);

  nostr_nip46_session_free(rpc_session);

  if (rc != 0 || !user_pubkey_hex) {
    g_warning("[NIP46_LOGIN] get_public_key RPC failed: %d", rc);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "get_public_key RPC failed");
    return;
  }

  g_message("[NIP46_LOGIN] get_public_key RPC returned: %s", user_pubkey_hex);
  g_task_return_pointer(task, user_pubkey_hex, free);
}

/* Callback when get_public_key RPC completes */
static void on_get_pubkey_done(GObject *source, GAsyncResult *result, gpointer user_data) {
  Nip46ConnectCtx *ctx = user_data;
  GError *error = NULL;

  char *user_pubkey_hex = g_task_propagate_pointer(G_TASK(result), &error);

  if (error) {
    g_warning("[NIP46_LOGIN] get_public_key async failed: %s", error->message);
    if (ctx->self && GNOSTR_IS_LOGIN(ctx->self)) {
      set_bunker_status(ctx->self, BUNKER_STATUS_ERROR,
                        "Failed to get user pubkey",
                        error->message);
    }
    g_clear_error(&error);
    nip46_connect_ctx_free(ctx);
    return;
  }

  /* Schedule final success handling on main thread */
  Nip46PubkeyCtx *pubkey_ctx = g_new0(Nip46PubkeyCtx, 1);
  pubkey_ctx->self = g_object_ref(ctx->self);
  pubkey_ctx->user_pubkey_hex = g_strdup(user_pubkey_hex);
  pubkey_ctx->signer_pubkey_hex = g_strdup(ctx->signer_pubkey_hex);
  pubkey_ctx->nostrconnect_uri = g_strdup(ctx->nostrconnect_uri);
  pubkey_ctx->nostrconnect_secret = g_strdup(ctx->nostrconnect_secret);

  free(user_pubkey_hex);
  nip46_connect_ctx_free(ctx);

  g_idle_add_full(G_PRIORITY_HIGH, on_nip46_pubkey_result, pubkey_ctx, nip46_pubkey_ctx_free);
}

/* Called on main thread after connect response - sets up session and spawns get_public_key RPC */
static gboolean on_nip46_connect_success(gpointer data) {
  Nip46ConnectCtx *ctx = data;

  g_message("[NIP46_LOGIN] *** on_nip46_connect_success ENTERED ***");

  if (!ctx || !ctx->self || !GNOSTR_IS_LOGIN(ctx->self)) {
    g_warning("[NIP46_LOGIN] on_nip46_connect_success: invalid context");
    nip46_connect_ctx_free(ctx);
    return G_SOURCE_REMOVE;
  }

  GnostrLogin *self = ctx->self;

  /* Stop the listener (we're not in the callback anymore) */
  stop_nip46_listener(self);

  g_message("[NIP46_LOGIN] Connect success!");
  g_message("[NIP46_LOGIN] Signer pubkey (communication): %s", ctx->signer_pubkey_hex);
  g_message("[NIP46_LOGIN] Spawning get_public_key RPC to get user's ACTUAL pubkey...");

  /* Update status to show we're getting the pubkey */
  set_bunker_status(self, BUNKER_STATUS_CONNECTING,
                    "Getting user identity...",
                    "Retrieving your public key from the signer");

  /* Create session for future signing operations */
  if (self->nip46_session) {
    nostr_nip46_session_free(self->nip46_session);
  }
  self->nip46_session = nostr_nip46_client_new();

  if (ctx->nostrconnect_uri) {
    nostr_nip46_client_connect(self->nip46_session, ctx->nostrconnect_uri, NULL);
  }
  if (ctx->nostrconnect_secret) {
    nostr_nip46_client_set_secret(self->nip46_session, ctx->nostrconnect_secret);
  }
  if (ctx->signer_pubkey_hex) {
    fprintf(stderr, "\n*** LOGIN: SETTING SIGNER PUBKEY ON SESSION ***\n");
    fprintf(stderr, "ctx->signer_pubkey_hex = %s\n", ctx->signer_pubkey_hex);
    nostr_nip46_client_set_signer_pubkey(self->nip46_session, ctx->signer_pubkey_hex);
  }

  /* Spawn async task for get_public_key RPC
   * We pass ownership of ctx to the task chain */
  GTask *task = g_task_new(NULL, self->cancellable, on_get_pubkey_done, ctx);
  g_task_set_task_data(task, ctx, NULL); /* ctx freed in callback, not here */
  g_task_run_in_thread(task, nip46_get_pubkey_thread);
  g_object_unref(task);

  return G_SOURCE_REMOVE;
}

static void gnostr_login_dispose(GObject *obj) {
  GnostrLogin *self = GNOSTR_LOGIN(obj);

  /* Disconnect name owner monitoring */
  if (self->name_owner_handler > 0) {
    NostrSignerProxy *proxy = gnostr_signer_proxy_get(NULL);
    if (proxy) {
      g_signal_handler_disconnect(proxy, self->name_owner_handler);
    }
    self->name_owner_handler = 0;
  }

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

  /* Debug: print relay count of session being returned */
  if (session) {
    char **relays = NULL;
    size_t n_relays = 0;
    nostr_nip46_session_get_relays(session, &relays, &n_relays);
    fprintf(stderr, "\n*** LOGIN TAKE SESSION ***\n");
    fprintf(stderr, "Session has %zu relays:\n", n_relays);
    for (size_t i = 0; i < n_relays && relays; i++) {
      fprintf(stderr, "  relay[%zu]: %s\n", i, relays[i] ? relays[i] : "(null)");
    }
    if (relays) {
      for (size_t i = 0; i < n_relays; i++) free(relays[i]);
      free(relays);
    }
  } else {
    fprintf(stderr, "\n*** LOGIN TAKE SESSION: NULL ***\n");
  }

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
  (void)source;
  (void)res;

  if (!GNOSTR_IS_LOGIN(self)) return;

  self->checking_local = FALSE;
  gtk_widget_set_visible(self->spinner_local, FALSE);

  GError *error = NULL;
  char *npub = NULL;
  NostrSignerProxy *proxy = gnostr_signer_proxy_get(&error);

  if (!proxy) {
    self->local_signer_available = FALSE;
    g_debug("[LOGIN] D-Bus proxy creation failed: %s",
            error ? error->message : "unknown");
    gtk_label_set_text(GTK_LABEL(self->lbl_local_status),
                       "No local signer");
    gtk_widget_set_sensitive(self->btn_local_signer, FALSE);
    g_clear_error(&error);
    return;
  }

  /* Monitor for daemon appearing/disappearing on D-Bus */
  if (self->name_owner_handler == 0) {
    self->name_owner_handler = g_signal_connect(
        proxy, "notify::g-name-owner",
        G_CALLBACK(on_name_owner_changed), self);
  }

  /* Check if a process actually owns the bus name before calling methods.
   * Without this check, calling GetPublicKey when no daemon is running
   * triggers D-Bus service activation which can timeout. */
  g_autofree gchar *name_owner =
      g_dbus_proxy_get_name_owner(G_DBUS_PROXY(proxy));
  if (!name_owner) {
    self->local_signer_available = FALSE;
    gtk_label_set_text(GTK_LABEL(self->lbl_local_status),
                       "Signer not running");
    gtk_widget_set_sensitive(self->btn_local_signer, FALSE);
    return;
  }

  /* Daemon is running - try GetPublicKey to verify it has a key */
  gboolean ok = nostr_org_nostr_signer_call_get_public_key_sync(
      proxy, &npub, NULL, &error);
  if (ok && npub && *npub) {
    self->local_signer_available = TRUE;
    gtk_label_set_text(GTK_LABEL(self->lbl_local_status), "Signer available");
    gtk_widget_set_sensitive(self->btn_local_signer, TRUE);
    g_free(npub);
  } else {
    /* Daemon is running but GetPublicKey failed - still allow local signing
     * so user can import a key through the signer daemon */
    self->local_signer_available = TRUE;
    g_debug("[LOGIN] Signer detected but GetPublicKey failed: %s",
            error ? error->message : "unknown");
    gtk_label_set_text(GTK_LABEL(self->lbl_local_status),
                       "Signer detected (no key configured)");
    gtk_widget_set_sensitive(self->btn_local_signer, TRUE);
    g_clear_error(&error);
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

/* Re-check signer availability when daemon appears/disappears on D-Bus */
static void on_name_owner_changed(GObject *proxy, GParamSpec *pspec,
                                   gpointer user_data) {
  (void)pspec;
  (void)proxy;
  GnostrLogin *self = GNOSTR_LOGIN(user_data);
  if (!GNOSTR_IS_LOGIN(self)) return;

  check_local_signer_availability(self);
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
    /* Check for specific D-Bus errors and show user-friendly messages */
    const char *msg = "Failed to get public key from local signer";
    if (error) {
      /* Check for NoKeyConfigured error from daemon */
      if (g_dbus_error_is_remote_error(error)) {
        gchar *remote_error = g_dbus_error_get_remote_error(error);
        if (remote_error && strstr(remote_error, "NoKeyConfigured")) {
          msg = "No key configured in local signer.\n\nPlease set up a key in GNostr Signer first.";
        } else {
          msg = error->message;
        }
        g_free(remote_error);
      } else {
        msg = error->message;
      }
    }
    show_toast(self, msg);
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
  const char *relay = NIP46_DEFAULT_RELAY;

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

  /* Start listening for NIP-46 responses in background (for QR flow)
   * but don't show intrusive "Waiting" status - let the QR speak for itself */
  const char *relay = NIP46_DEFAULT_RELAY;
  start_nip46_listener(self, relay);

  /* Keep status hidden - user sees the QR and the URI entry field
   * Status only appears when they click Connect or an error occurs */
  set_bunker_status(self, BUNKER_STATUS_IDLE, "", NULL);
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

  const char *relay = NIP46_DEFAULT_RELAY;
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
  char *client_secret;  /* Generated ephemeral client key for this session */
} BunkerConnectCtx;

static void bunker_connect_ctx_free(gpointer data) {
  BunkerConnectCtx *ctx = (BunkerConnectCtx*)data;
  if (!ctx) return;
  g_free(ctx->bunker_uri);
  if (ctx->client_secret) {
    memset(ctx->client_secret, 0, strlen(ctx->client_secret));  /* Wipe secret */
    free(ctx->client_secret);
  }
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

  g_message("[NIP46_LOGIN] Starting bunker connect: %.40s...", ctx->bunker_uri);

  /* Step 1: Parse bunker:// URI to extract signer pubkey, relays, and connect_secret */
  NostrNip46BunkerURI parsed = {0};
  if (nostr_nip46_uri_parse_bunker(ctx->bunker_uri, &parsed) != 0) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Invalid bunker URI format");
    return;
  }

  if (!parsed.remote_signer_pubkey_hex || !parsed.relays || parsed.n_relays == 0) {
    nostr_nip46_uri_bunker_free(&parsed);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Bunker URI missing required fields (pubkey or relay)");
    return;
  }

  g_message("[NIP46_LOGIN] Parsed URI: signer=%.16s..., %zu relays, secret=%s",
            parsed.remote_signer_pubkey_hex, parsed.n_relays,
            parsed.secret ? "present" : "none");

  /* Step 2: Generate ephemeral client keypair for this session */
  char *client_secret = nostr_key_generate_private();
  if (!client_secret) {
    nostr_nip46_uri_bunker_free(&parsed);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to generate client keypair");
    return;
  }

  /* Step 3: Create NIP-46 session and configure it */
  NostrNip46Session *session = nostr_nip46_client_new();
  if (!session) {
    free(client_secret);
    nostr_nip46_uri_bunker_free(&parsed);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to create NIP-46 session");
    return;
  }

  /* Parse URI into session to set remote_pubkey_hex and relays */
  int rc = nostr_nip46_client_connect(session, ctx->bunker_uri, NULL);
  if (rc != 0) {
    free(client_secret);
    nostr_nip46_session_free(session);
    nostr_nip46_uri_bunker_free(&parsed);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to parse bunker URI into session");
    return;
  }

  /* Set the CLIENT's secret key (not the URI's secret= which is the connect token) */
  if (nostr_nip46_client_set_secret(session, client_secret) != 0) {
    free(client_secret);
    nostr_nip46_session_free(session);
    nostr_nip46_uri_bunker_free(&parsed);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to set client secret");
    return;
  }

  g_message("[NIP46_LOGIN] Session configured, sending connect RPC...");

  /* Step 4: Send "connect" RPC to remote signer */
  char *connect_result = NULL;
  rc = nostr_nip46_client_connect_rpc(session, parsed.secret, "sign_event", &connect_result);
  if (rc != 0) {
    free(client_secret);
    nostr_nip46_session_free(session);
    nostr_nip46_uri_bunker_free(&parsed);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Connect RPC failed - signer did not respond");
    return;
  }

  g_message("[NIP46_LOGIN] Connect RPC success: %s", connect_result);

  /* Validate connect response (should be "ack" or the connect_secret) */
  if (strcmp(connect_result, "ack") != 0 &&
      (!parsed.secret || strcmp(connect_result, parsed.secret) != 0)) {
    g_warning("[NIP46_LOGIN] Unexpected connect response: %s", connect_result);
    /* Continue anyway - some signers may return different values */
  }
  free(connect_result);

  /* Step 5: Send "get_public_key" RPC to get actual user pubkey */
  g_message("[NIP46_LOGIN] Sending get_public_key RPC...");
  char *pubkey_hex = NULL;
  rc = nostr_nip46_client_get_public_key_rpc(session, &pubkey_hex);
  if (rc != 0 || !pubkey_hex) {
    free(client_secret);
    nostr_nip46_session_free(session);
    nostr_nip46_uri_bunker_free(&parsed);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to get public key from signer");
    return;
  }

  g_message("[NIP46_LOGIN] Got user pubkey: %.16s...", pubkey_hex);

  /* Store the client secret for later signing operations */
  ctx->client_secret = client_secret; /* Transfer ownership */

  nostr_nip46_uri_bunker_free(&parsed);

  /* Convert hex pubkey to npub */
  uint8_t pubkey_bytes[32];
  for (int i = 0; i < 32; i++) {
    unsigned int byte;
    if (sscanf(pubkey_hex + i * 2, "%2x", &byte) != 1) {
      free(pubkey_hex);
      nostr_nip46_session_free(session);
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                              "Invalid pubkey format from signer");
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

  g_message("[NIP46_LOGIN] Bunker connect SUCCESS: %s", npub);

  /* Save credentials to settings for session persistence */
  {
    char *secret_hex = NULL;
    char *signer_pubkey = NULL;
    char **relays = NULL;
    size_t n_relays = 0;

    nostr_nip46_session_get_secret(session, &secret_hex);
    nostr_nip46_session_get_remote_pubkey(session, &signer_pubkey);
    nostr_nip46_session_get_relays(session, &relays, &n_relays);

    fprintf(stderr, "\n*** SAVING CREDENTIALS FROM BUNKER LOGIN ***\n");
    fprintf(stderr, "Session has %zu relays before save:\n", n_relays);
    for (size_t i = 0; i < n_relays && relays; i++) {
      fprintf(stderr, "  relay[%zu]: %s\n", i, relays[i] ? relays[i] : "(null)");
    }

    save_nip46_credentials_to_settings(secret_hex, signer_pubkey, relays, n_relays);

    free(secret_hex);
    free(signer_pubkey);
    if (relays) {
      for (size_t i = 0; i < n_relays; i++) free(relays[i]);
      free(relays);
    }
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

  /* We hold a ref, so object is valid. But check if it's been disposed. */
  if (!GNOSTR_IS_LOGIN(self)) {
    g_object_unref(self);
    return;
  }

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
    g_object_unref(self);
    return;
  }

  if (!result || !result[0]) {
    set_bunker_status(self, BUNKER_STATUS_ERROR,
                      "Connection failed",
                      "The signer did not respond. Try again.");
    g_object_unref(self);
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

  g_object_unref(self);
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

  /* Ref self to prevent use-after-free if dialog closes before task completes */
  GTask *task = g_task_new(NULL, self->cancellable, bunker_connect_complete, g_object_ref(self));
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
                                                char **relays,
                                                size_t n_relays) {
  GSettings *settings = g_settings_new(SETTINGS_SCHEMA_CLIENT);
  if (!settings) return;

  g_settings_set_string(settings, "nip46-client-secret",
                        client_secret_hex ? client_secret_hex : "");
  g_settings_set_string(settings, "nip46-signer-pubkey",
                        signer_pubkey_hex ? signer_pubkey_hex : "");

  /* Build GVariant array of relay strings */
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE_STRING_ARRAY);
  for (size_t i = 0; i < n_relays && relays; i++) {
    if (relays[i] && *relays[i]) {
      g_variant_builder_add(&builder, "s", relays[i]);
    }
  }
  g_settings_set_value(settings, "nip46-relays", g_variant_builder_end(&builder));

  g_message("[NIP46_LOGIN] Saved NIP-46 credentials to settings (secret: %zu chars, pubkey: %s, relays: %zu)",
            client_secret_hex ? strlen(client_secret_hex) : 0,
            signer_pubkey_hex ? signer_pubkey_hex : "(null)",
            n_relays);

  g_object_unref(settings);
}

static void show_success(GnostrLogin *self, const char *npub) {
  /* Emit signed-in signal first */
  g_signal_emit(self, signals[SIGNAL_SIGNED_IN], 0, npub);

  /* Close the dialog automatically - user doesn't need to click "Done" */
  GtkWidget *win = gtk_widget_get_ancestor(GTK_WIDGET(self), GTK_TYPE_WINDOW);
  if (win && GTK_IS_WINDOW(win)) {
    gtk_window_close(GTK_WINDOW(win));
  }
}

static void on_close_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrLogin *self = GNOSTR_LOGIN(user_data);
  if (!GNOSTR_IS_LOGIN(self)) return;

  g_signal_emit(self, signals[SIGNAL_CANCELLED], 0);
  /* Close the parent window (GnostrLogin is an AdwBin, not a GtkWindow) */
  GtkWidget *win = gtk_widget_get_ancestor(GTK_WIDGET(self), GTK_TYPE_WINDOW);
  if (win && GTK_IS_WINDOW(win)) {
    gtk_window_close(GTK_WINDOW(win));
  }
}

static void on_done_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrLogin *self = GNOSTR_LOGIN(user_data);
  if (!GNOSTR_IS_LOGIN(self)) return;

  /* Close the parent window (GnostrLogin is an AdwBin, not a GtkWindow) */
  GtkWidget *win = gtk_widget_get_ancestor(GTK_WIDGET(self), GTK_TYPE_WINDOW);
  if (win && GTK_IS_WINDOW(win)) {
    gtk_window_close(GTK_WINDOW(win));
  }
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

    /* Get the connect result - should be "ack" or may match our connect secret */
    char *result = NULL;
    if (nostr_json_get_string(plaintext, "result", &result) != 0 || !result) {
      g_warning("[NIP46_LOGIN] No result in NIP-46 response");
      free(result);
      g_free(plaintext);
      continue;
    }

    g_message("[NIP46_LOGIN] Connect response result: %s", result);

    /* Validate the connect response:
     * - "ack" means simple acknowledgment
     * - A 64-char hex may be the connect secret (we validate it matches)
     * The signer's communication pubkey is always the sender_pubkey. */
    gboolean connect_valid = FALSE;
    if (strcmp(result, "ack") == 0) {
      connect_valid = TRUE;
      g_message("[NIP46_LOGIN] Connect acknowledged with 'ack'");
    } else if (self->nostrconnect_secret && strlen(result) == 64 &&
               strcmp(result, self->nostrconnect_secret) == 0) {
      /* Result matches our connect secret - this is valid per NIP-46 */
      connect_valid = TRUE;
      g_message("[NIP46_LOGIN] Connect acknowledged with matching secret");
    } else if (strlen(result) == 64) {
      /* Some signers return the secret, accept it even if we can't verify */
      connect_valid = TRUE;
      g_message("[NIP46_LOGIN] Connect acknowledged with 64-char result (assuming valid)");
    } else {
      g_warning("[NIP46_LOGIN] Unexpected connect result format: %s", result);
    }

    free(result);
    g_free(plaintext);

    if (!connect_valid) {
      continue;
    }

    /* The signer's communication pubkey is ALWAYS the sender of the event.
     * This is NOT the user's pubkey - we need to call get_public_key RPC for that. */
    g_message("[NIP46_LOGIN] Signer communication pubkey (sender): %s", sender_pubkey);

    /* Defer connect success handling - will call get_public_key RPC to get
     * the user's ACTUAL pubkey (which may differ from signer communication key) */
    Nip46ConnectCtx *ctx = g_new0(Nip46ConnectCtx, 1);
    ctx->self = g_object_ref(self);
    ctx->signer_pubkey_hex = g_strdup(sender_pubkey);
    ctx->nostrconnect_uri = g_strdup(self->nostrconnect_uri);
    ctx->nostrconnect_secret = g_strdup(self->nostrconnect_secret);

    /* Extract relay URL from the nostrconnect URI, fall back to default */
    ctx->relay_url = NULL;
    if (self->nostrconnect_uri) {
      NostrNip46ConnectURI parsed_uri = {0};
      if (nostr_nip46_uri_parse_connect(self->nostrconnect_uri, &parsed_uri) == 0) {
        if (parsed_uri.n_relays > 0 && parsed_uri.relays && parsed_uri.relays[0]) {
          ctx->relay_url = g_strdup(parsed_uri.relays[0]);
        }
        nostr_nip46_uri_connect_free(&parsed_uri);
      }
    }
    if (!ctx->relay_url) {
      ctx->relay_url = g_strdup(NIP46_DEFAULT_RELAY);
    }

    g_idle_add_full(G_PRIORITY_HIGH, on_nip46_connect_success, ctx, NULL);
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
