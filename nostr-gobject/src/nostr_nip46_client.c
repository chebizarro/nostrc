/* hq-8p6sw: GObject wrapper for NIP-46 remote signer client.
 *
 * Wraps NostrNip46Session (client mode) as a GObject with properties,
 * signals, and GTask-based async methods for GTK main-loop integration.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Core NIP-46 headers first */
#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_types.h"

/* GObject wrapper header */
#include "nostr_nip46_client.h"
#include <glib.h>
#include <gio/gio.h>

/* ── Property IDs ──────────────────────────────────────────────── */

enum {
    PROP_0,
    PROP_BUNKER_URI,
    PROP_STATE,
    PROP_REMOTE_PUBKEY,
    PROP_TIMEOUT,
    N_PROPERTIES
};

static GParamSpec *obj_properties[N_PROPERTIES] = { NULL, };

/* ── Signal IDs ────────────────────────────────────────────────── */

enum {
    SIGNAL_STATE_CHANGED,
    SIGNAL_ERROR,
    N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

/* ── Instance ──────────────────────────────────────────────────── */

struct _GNostrNip46Client {
    GObject parent_instance;

    NostrNip46Session *session;     /* Core NIP-46 session (owned) */
    gchar *bunker_uri;              /* Cached bunker URI string */
    gchar *remote_pubkey;           /* Cached remote signer pubkey hex */
    GNostrNip46State state;         /* Cached GObject state */
};

G_DEFINE_TYPE(GNostrNip46Client, gnostr_nip46_client, G_TYPE_OBJECT)

/* ── Helpers ───────────────────────────────────────────────────── */

static GNostrNip46State
core_state_to_gobject(NostrNip46State core)
{
    switch (core) {
    case NOSTR_NIP46_STATE_DISCONNECTED: return GNOSTR_NIP46_STATE_DISCONNECTED;
    case NOSTR_NIP46_STATE_CONNECTING:   return GNOSTR_NIP46_STATE_CONNECTING;
    case NOSTR_NIP46_STATE_CONNECTED:    return GNOSTR_NIP46_STATE_CONNECTED;
    case NOSTR_NIP46_STATE_STOPPING:     return GNOSTR_NIP46_STATE_STOPPING;
    default:                             return GNOSTR_NIP46_STATE_DISCONNECTED;
    }
}

static void
update_state(GNostrNip46Client *self)
{
    if (!self->session)
        return;

    GNostrNip46State new_state = core_state_to_gobject(
        nostr_nip46_client_get_state_public(self->session));

    if (self->state == new_state)
        return;

    GNostrNip46State old_state = self->state;
    self->state = new_state;

    g_signal_emit(self, signals[SIGNAL_STATE_CHANGED], 0, old_state, new_state);
    g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_STATE]);
}

static void
update_remote_pubkey(GNostrNip46Client *self)
{
    if (!self->session)
        return;

    char *pk = NULL;
    if (nostr_nip46_session_get_remote_pubkey(self->session, &pk) == 0 && pk) {
        g_free(self->remote_pubkey);
        self->remote_pubkey = g_strdup(pk);
        free(pk);
        g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_REMOTE_PUBKEY]);
    }
}

/* ── GObject vfuncs ────────────────────────────────────────────── */

static void
gnostr_nip46_client_get_property(GObject    *object,
                                  guint       property_id,
                                  GValue     *value,
                                  GParamSpec *pspec)
{
    GNostrNip46Client *self = GNOSTR_NIP46_CLIENT(object);

    switch (property_id) {
    case PROP_BUNKER_URI:
        g_value_set_string(value, self->bunker_uri);
        break;
    case PROP_STATE:
        g_value_set_enum(value, self->state);
        break;
    case PROP_REMOTE_PUBKEY:
        g_value_set_string(value, self->remote_pubkey);
        break;
    case PROP_TIMEOUT:
        g_value_set_uint(value,
            self->session ? nostr_nip46_client_get_timeout(self->session) : 30000);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
gnostr_nip46_client_set_property(GObject      *object,
                                  guint         property_id,
                                  const GValue *value,
                                  GParamSpec   *pspec)
{
    GNostrNip46Client *self = GNOSTR_NIP46_CLIENT(object);

    switch (property_id) {
    case PROP_TIMEOUT:
        gnostr_nip46_client_set_timeout(self, g_value_get_uint(value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
gnostr_nip46_client_finalize(GObject *object)
{
    GNostrNip46Client *self = GNOSTR_NIP46_CLIENT(object);

    if (self->session) {
        nostr_nip46_client_stop(self->session);
        nostr_nip46_session_free(self->session);
        self->session = NULL;
    }

    g_free(self->bunker_uri);
    g_free(self->remote_pubkey);

    G_OBJECT_CLASS(gnostr_nip46_client_parent_class)->finalize(object);
}

static void
gnostr_nip46_client_class_init(GNostrNip46ClientClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->get_property = gnostr_nip46_client_get_property;
    object_class->set_property = gnostr_nip46_client_set_property;
    object_class->finalize = gnostr_nip46_client_finalize;

    obj_properties[PROP_BUNKER_URI] =
        g_param_spec_string("bunker-uri",
                            "Bunker URI",
                            "The bunker:// or nostrconnect:// URI",
                            NULL,
                            G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                            G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_STATE] =
        g_param_spec_enum("state",
                          "State",
                          "Current NIP-46 session state",
                          GNOSTR_TYPE_NIP46_STATE,
                          GNOSTR_NIP46_STATE_DISCONNECTED,
                          G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_REMOTE_PUBKEY] =
        g_param_spec_string("remote-pubkey",
                            "Remote Pubkey",
                            "Remote signer pubkey hex",
                            NULL,
                            G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY |
                            G_PARAM_STATIC_STRINGS);

    obj_properties[PROP_TIMEOUT] =
        g_param_spec_uint("timeout",
                          "Timeout",
                          "RPC request timeout in milliseconds",
                          0, G_MAXUINT, 30000,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPERTIES, obj_properties);

    /**
     * GNostrNip46Client::state-changed:
     * @self: the client
     * @old_state: previous #GNostrNip46State
     * @new_state: new #GNostrNip46State
     *
     * Emitted when the NIP-46 session state changes.
     */
    signals[SIGNAL_STATE_CHANGED] =
        g_signal_new("state-changed",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 2,
                     GNOSTR_TYPE_NIP46_STATE,
                     GNOSTR_TYPE_NIP46_STATE);

    /**
     * GNostrNip46Client::error:
     * @self: the client
     * @error: a #GError describing the error
     *
     * Emitted when an RPC or connection error occurs.
     */
    signals[SIGNAL_ERROR] =
        g_signal_new("error",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 1, G_TYPE_ERROR);
}

static void
gnostr_nip46_client_init(GNostrNip46Client *self)
{
    self->session = nostr_nip46_client_new();
    self->state = GNOSTR_NIP46_STATE_DISCONNECTED;
}

/* ── Public API ────────────────────────────────────────────────── */

GNostrNip46Client *
gnostr_nip46_client_new(void)
{
    return g_object_new(GNOSTR_TYPE_NIP46_CLIENT, NULL);
}

gboolean
gnostr_nip46_client_connect_to_bunker(GNostrNip46Client *self,
                                       const gchar *bunker_uri,
                                       const gchar *perms,
                                       GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_NIP46_CLIENT(self), FALSE);
    g_return_val_if_fail(bunker_uri != NULL, FALSE);
    g_return_val_if_fail(self->session != NULL, FALSE);

    int rc = nostr_nip46_client_connect(self->session, bunker_uri, perms);
    if (rc != 0) {
        g_set_error(error, NOSTR_ERROR, NOSTR_ERROR_CONNECTION_FAILED,
                    "Failed to parse bunker URI: %s", bunker_uri);
        return FALSE;
    }

    g_free(self->bunker_uri);
    self->bunker_uri = g_strdup(bunker_uri);
    g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_BUNKER_URI]);

    update_remote_pubkey(self);

    return TRUE;
}

gboolean
gnostr_nip46_client_start(GNostrNip46Client *self, GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_NIP46_CLIENT(self), FALSE);
    g_return_val_if_fail(self->session != NULL, FALSE);

    int rc = nostr_nip46_client_start(self->session);
    if (rc != 0) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_CONNECTION_FAILED,
                            "Failed to start NIP-46 client relay pool");
        return FALSE;
    }

    update_state(self);
    return TRUE;
}

/* ── Async start ───────────────────────────────────────────────── */

static void
start_thread(GTask        *task,
             gpointer      source_object,
             gpointer      task_data G_GNUC_UNUSED,
             GCancellable *cancellable)
{
    GNostrNip46Client *self = GNOSTR_NIP46_CLIENT(source_object);
    GError *error = NULL;

    if (g_cancellable_set_error_if_cancelled(cancellable, &error)) {
        g_task_return_error(task, error);
        return;
    }

    if (gnostr_nip46_client_start(self, &error)) {
        g_task_return_boolean(task, TRUE);
    } else {
        g_task_return_error(task, error);
    }
}

void
gnostr_nip46_client_start_async(GNostrNip46Client   *self,
                                 GCancellable        *cancellable,
                                 GAsyncReadyCallback  callback,
                                 gpointer             user_data)
{
    g_return_if_fail(GNOSTR_IS_NIP46_CLIENT(self));

    GTask *task = g_task_new(self, cancellable, callback, user_data);
    g_task_set_source_tag(task, gnostr_nip46_client_start_async);
    g_task_run_in_thread(task, start_thread);
    g_object_unref(task);
}

gboolean
gnostr_nip46_client_start_finish(GNostrNip46Client *self,
                                  GAsyncResult      *result,
                                  GError           **error)
{
    g_return_val_if_fail(GNOSTR_IS_NIP46_CLIENT(self), FALSE);
    g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

    return g_task_propagate_boolean(G_TASK(result), error);
}

void
gnostr_nip46_client_stop(GNostrNip46Client *self)
{
    g_return_if_fail(GNOSTR_IS_NIP46_CLIENT(self));

    if (self->session) {
        nostr_nip46_client_stop(self->session);
        update_state(self);
    }
}

/* ── Sync RPC methods ──────────────────────────────────────────── */

gboolean
gnostr_nip46_client_connect_rpc(GNostrNip46Client *self,
                                 const gchar *connect_secret,
                                 const gchar *perms,
                                 gchar **out_result,
                                 GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_NIP46_CLIENT(self), FALSE);
    g_return_val_if_fail(self->session != NULL, FALSE);

    char *result = NULL;
    int rc = nostr_nip46_client_connect_rpc(self->session, connect_secret, perms, &result);
    if (rc != 0) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_CONNECTION_FAILED,
                            "NIP-46 connect RPC failed");
        return FALSE;
    }

    if (out_result)
        *out_result = g_strdup(result);
    free(result);

    update_remote_pubkey(self);
    update_state(self);
    return TRUE;
}

gboolean
gnostr_nip46_client_get_public_key_rpc(GNostrNip46Client *self,
                                        gchar **out_pubkey_hex,
                                        GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_NIP46_CLIENT(self), FALSE);
    g_return_val_if_fail(self->session != NULL, FALSE);

    char *pubkey = NULL;
    int rc = nostr_nip46_client_get_public_key_rpc(self->session, &pubkey);
    if (rc != 0) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_KEY,
                            "NIP-46 get_public_key RPC failed");
        return FALSE;
    }

    if (out_pubkey_hex)
        *out_pubkey_hex = g_strdup(pubkey);
    free(pubkey);
    return TRUE;
}

gboolean
gnostr_nip46_client_sign_event(GNostrNip46Client *self,
                                const gchar *event_json,
                                gchar **out_signed_json,
                                GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_NIP46_CLIENT(self), FALSE);
    g_return_val_if_fail(event_json != NULL, FALSE);
    g_return_val_if_fail(self->session != NULL, FALSE);

    char *signed_json = NULL;
    int rc = nostr_nip46_client_sign_event(self->session, event_json, &signed_json);
    if (rc != 0) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_SIGNATURE_FAILED,
                            "NIP-46 sign_event RPC failed");
        return FALSE;
    }

    if (out_signed_json)
        *out_signed_json = g_strdup(signed_json);
    free(signed_json);
    return TRUE;
}

gboolean
gnostr_nip46_client_ping(GNostrNip46Client *self, GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_NIP46_CLIENT(self), FALSE);
    g_return_val_if_fail(self->session != NULL, FALSE);

    int rc = nostr_nip46_client_ping(self->session);
    if (rc != 0) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_TIMEOUT,
                            "NIP-46 ping RPC failed");
        return FALSE;
    }

    return TRUE;
}

gboolean
gnostr_nip46_client_nip04_encrypt(GNostrNip46Client *self,
                                   const gchar *peer_pubkey_hex,
                                   const gchar *plaintext,
                                   gchar **out_ciphertext,
                                   GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_NIP46_CLIENT(self), FALSE);
    g_return_val_if_fail(self->session != NULL, FALSE);

    char *ct = NULL;
    /* nostrc-u1qh: Use _rpc variant to delegate to remote signer (not local transport key) */
    int rc = nostr_nip46_client_nip04_encrypt_rpc(self->session, peer_pubkey_hex, plaintext, &ct);
    if (rc != 0) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_ENCRYPTION_FAILED,
                            "NIP-46 nip04_encrypt RPC failed");
        return FALSE;
    }

    if (out_ciphertext)
        *out_ciphertext = g_strdup(ct);
    free(ct);
    return TRUE;
}

gboolean
gnostr_nip46_client_nip04_decrypt(GNostrNip46Client *self,
                                   const gchar *peer_pubkey_hex,
                                   const gchar *ciphertext,
                                   gchar **out_plaintext,
                                   GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_NIP46_CLIENT(self), FALSE);
    g_return_val_if_fail(self->session != NULL, FALSE);

    char *pt = NULL;
    /* nostrc-u1qh: Use _rpc variant to delegate to remote signer (not local transport key) */
    int rc = nostr_nip46_client_nip04_decrypt_rpc(self->session, peer_pubkey_hex, ciphertext, &pt);
    if (rc != 0) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_DECRYPTION_FAILED,
                            "NIP-46 nip04_decrypt RPC failed");
        return FALSE;
    }

    if (out_plaintext)
        *out_plaintext = g_strdup(pt);
    free(pt);
    return TRUE;
}

gboolean
gnostr_nip46_client_nip44_encrypt(GNostrNip46Client *self,
                                   const gchar *peer_pubkey_hex,
                                   const gchar *plaintext,
                                   gchar **out_ciphertext,
                                   GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_NIP46_CLIENT(self), FALSE);
    g_return_val_if_fail(self->session != NULL, FALSE);

    char *ct = NULL;
    /* nostrc-u1qh: Use _rpc variant to delegate to remote signer (not local transport key) */
    int rc = nostr_nip46_client_nip44_encrypt_rpc(self->session, peer_pubkey_hex, plaintext, &ct);
    if (rc != 0) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_ENCRYPTION_FAILED,
                            "NIP-46 nip44_encrypt RPC failed");
        return FALSE;
    }

    if (out_ciphertext)
        *out_ciphertext = g_strdup(ct);
    free(ct);
    return TRUE;
}

gboolean
gnostr_nip46_client_nip44_decrypt(GNostrNip46Client *self,
                                   const gchar *peer_pubkey_hex,
                                   const gchar *ciphertext,
                                   gchar **out_plaintext,
                                   GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_NIP46_CLIENT(self), FALSE);
    g_return_val_if_fail(self->session != NULL, FALSE);

    char *pt = NULL;
    /* nostrc-u1qh: Use _rpc variant to delegate to remote signer (not local transport key) */
    int rc = nostr_nip46_client_nip44_decrypt_rpc(self->session, peer_pubkey_hex, ciphertext, &pt);
    if (rc != 0) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_DECRYPTION_FAILED,
                            "NIP-46 nip44_decrypt RPC failed");
        return FALSE;
    }

    if (out_plaintext)
        *out_plaintext = g_strdup(pt);
    free(pt);
    return TRUE;
}

/* ── Async RPC: connect_rpc ────────────────────────────────────── */

typedef struct {
    gchar *connect_secret;
    gchar *perms;
} ConnectRpcAsyncData;

static void
connect_rpc_async_data_free(gpointer data)
{
    ConnectRpcAsyncData *d = data;
    g_free(d->connect_secret);
    g_free(d->perms);
    g_free(d);
}

static void
connect_rpc_thread(GTask        *task,
                   gpointer      source_object,
                   gpointer      task_data,
                   GCancellable *cancellable)
{
    GNostrNip46Client *self = GNOSTR_NIP46_CLIENT(source_object);
    ConnectRpcAsyncData *d = task_data;
    GError *error = NULL;

    if (g_cancellable_set_error_if_cancelled(cancellable, &error)) {
        g_task_return_error(task, error);
        return;
    }

    gchar *result = NULL;
    if (gnostr_nip46_client_connect_rpc(self, d->connect_secret, d->perms,
                                         &result, &error)) {
        g_task_return_pointer(task, result, g_free);
    } else {
        g_task_return_error(task, error);
    }
}

void
gnostr_nip46_client_connect_rpc_async(GNostrNip46Client   *self,
                                       const gchar         *connect_secret,
                                       const gchar         *perms,
                                       GCancellable        *cancellable,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data)
{
    g_return_if_fail(GNOSTR_IS_NIP46_CLIENT(self));

    GTask *task = g_task_new(self, cancellable, callback, user_data);
    g_task_set_source_tag(task, gnostr_nip46_client_connect_rpc_async);

    ConnectRpcAsyncData *d = g_new0(ConnectRpcAsyncData, 1);
    d->connect_secret = g_strdup(connect_secret);
    d->perms = g_strdup(perms);
    g_task_set_task_data(task, d, connect_rpc_async_data_free);

    g_task_run_in_thread(task, connect_rpc_thread);
    g_object_unref(task);
}

gchar *
gnostr_nip46_client_connect_rpc_finish(GNostrNip46Client *self,
                                        GAsyncResult      *result,
                                        GError           **error)
{
    g_return_val_if_fail(GNOSTR_IS_NIP46_CLIENT(self), NULL);
    g_return_val_if_fail(g_task_is_valid(result, self), NULL);

    return g_task_propagate_pointer(G_TASK(result), error);
}

/* ── Async RPC: get_public_key_rpc ─────────────────────────────── */

static void
get_pubkey_rpc_thread(GTask        *task,
                      gpointer      source_object,
                      gpointer      task_data G_GNUC_UNUSED,
                      GCancellable *cancellable)
{
    GNostrNip46Client *self = GNOSTR_NIP46_CLIENT(source_object);
    GError *error = NULL;

    if (g_cancellable_set_error_if_cancelled(cancellable, &error)) {
        g_task_return_error(task, error);
        return;
    }

    gchar *pubkey = NULL;
    if (gnostr_nip46_client_get_public_key_rpc(self, &pubkey, &error)) {
        g_task_return_pointer(task, pubkey, g_free);
    } else {
        g_task_return_error(task, error);
    }
}

void
gnostr_nip46_client_get_public_key_rpc_async(GNostrNip46Client   *self,
                                              GCancellable        *cancellable,
                                              GAsyncReadyCallback  callback,
                                              gpointer             user_data)
{
    g_return_if_fail(GNOSTR_IS_NIP46_CLIENT(self));

    GTask *task = g_task_new(self, cancellable, callback, user_data);
    g_task_set_source_tag(task, gnostr_nip46_client_get_public_key_rpc_async);
    g_task_run_in_thread(task, get_pubkey_rpc_thread);
    g_object_unref(task);
}

gchar *
gnostr_nip46_client_get_public_key_rpc_finish(GNostrNip46Client *self,
                                               GAsyncResult      *result,
                                               GError           **error)
{
    g_return_val_if_fail(GNOSTR_IS_NIP46_CLIENT(self), NULL);
    g_return_val_if_fail(g_task_is_valid(result, self), NULL);

    return g_task_propagate_pointer(G_TASK(result), error);
}

/* ── Async RPC: sign_event ─────────────────────────────────────── */

typedef struct {
    gchar *event_json;
} SignEventAsyncData;

static void
sign_event_async_data_free(gpointer data)
{
    SignEventAsyncData *d = data;
    g_free(d->event_json);
    g_free(d);
}

static void
sign_event_thread(GTask        *task,
                  gpointer      source_object,
                  gpointer      task_data,
                  GCancellable *cancellable)
{
    GNostrNip46Client *self = GNOSTR_NIP46_CLIENT(source_object);
    SignEventAsyncData *d = task_data;
    GError *error = NULL;

    if (g_cancellable_set_error_if_cancelled(cancellable, &error)) {
        g_task_return_error(task, error);
        return;
    }

    gchar *signed_json = NULL;
    if (gnostr_nip46_client_sign_event(self, d->event_json, &signed_json, &error)) {
        g_task_return_pointer(task, signed_json, g_free);
    } else {
        g_task_return_error(task, error);
    }
}

void
gnostr_nip46_client_sign_event_async(GNostrNip46Client   *self,
                                      const gchar         *event_json,
                                      GCancellable        *cancellable,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data)
{
    g_return_if_fail(GNOSTR_IS_NIP46_CLIENT(self));
    g_return_if_fail(event_json != NULL);

    GTask *task = g_task_new(self, cancellable, callback, user_data);
    g_task_set_source_tag(task, gnostr_nip46_client_sign_event_async);

    SignEventAsyncData *d = g_new0(SignEventAsyncData, 1);
    d->event_json = g_strdup(event_json);
    g_task_set_task_data(task, d, sign_event_async_data_free);

    g_task_run_in_thread(task, sign_event_thread);
    g_object_unref(task);
}

gchar *
gnostr_nip46_client_sign_event_finish(GNostrNip46Client *self,
                                       GAsyncResult      *result,
                                       GError           **error)
{
    g_return_val_if_fail(GNOSTR_IS_NIP46_CLIENT(self), NULL);
    g_return_val_if_fail(g_task_is_valid(result, self), NULL);

    return g_task_propagate_pointer(G_TASK(result), error);
}

/* ── Configuration ─────────────────────────────────────────────── */

void
gnostr_nip46_client_set_timeout(GNostrNip46Client *self, guint timeout_ms)
{
    g_return_if_fail(GNOSTR_IS_NIP46_CLIENT(self));
    g_return_if_fail(self->session != NULL);

    guint old = nostr_nip46_client_get_timeout(self->session);
    nostr_nip46_client_set_timeout(self->session, (uint32_t)timeout_ms);

    if (old != timeout_ms)
        g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_TIMEOUT]);
}

guint
gnostr_nip46_client_get_timeout(GNostrNip46Client *self)
{
    g_return_val_if_fail(GNOSTR_IS_NIP46_CLIENT(self), 30000);

    if (self->session)
        return (guint)nostr_nip46_client_get_timeout(self->session);
    return 30000;
}

/* ── Property Accessors ───────────────────────────────────────── */

GNostrNip46State
gnostr_nip46_client_get_state(GNostrNip46Client *self)
{
    g_return_val_if_fail(GNOSTR_IS_NIP46_CLIENT(self), GNOSTR_NIP46_STATE_DISCONNECTED);

    if (self->session)
        update_state(self);
    return self->state;
}

const gchar *
gnostr_nip46_client_get_bunker_uri(GNostrNip46Client *self)
{
    g_return_val_if_fail(GNOSTR_IS_NIP46_CLIENT(self), NULL);
    return self->bunker_uri;
}

const gchar *
gnostr_nip46_client_get_remote_pubkey(GNostrNip46Client *self)
{
    g_return_val_if_fail(GNOSTR_IS_NIP46_CLIENT(self), NULL);
    return self->remote_pubkey;
}

NostrNip46Session *
gnostr_nip46_client_get_session(GNostrNip46Client *self)
{
    g_return_val_if_fail(GNOSTR_IS_NIP46_CLIENT(self), NULL);
    return self->session;
}
