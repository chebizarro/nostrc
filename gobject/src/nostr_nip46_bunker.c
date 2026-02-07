/* hq-8p6sw: GObject wrapper for NIP-46 bunker (remote signer service).
 *
 * Replaces C function pointer callbacks with GObject signals:
 *   - "authorize-request" → replaces NostrNip46AuthorizeFn
 *   - "sign-request"      → replaces NostrNip46SignFn
 *
 * Signal handlers run synchronously on the bunker's dispatch thread,
 * so they must return quickly. For async authorization UIs, use
 * handle_cipher from a GTask thread and block on user input there.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/* Core NIP-46 headers first */
#include "nostr/nip46/nip46_bunker.h"
#include "nostr/nip46/nip46_types.h"

/* GObject wrapper header */
#include "nostr_nip46_bunker.h"
#include <glib.h>
#include <gio/gio.h>
#include <string.h>

/* ── Signal IDs ────────────────────────────────────────────────── */

enum {
    SIGNAL_AUTHORIZE_REQUEST,
    SIGNAL_SIGN_REQUEST,
    SIGNAL_ERROR,
    N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

/* ── Instance ──────────────────────────────────────────────────── */

struct _GNostrNip46Bunker {
    GObject parent_instance;
    NostrNip46Session *session;     /* Core NIP-46 session (owned) */
};

G_DEFINE_TYPE(GNostrNip46Bunker, gnostr_nip46_bunker, G_TYPE_OBJECT)

/* ── Signal-based callbacks bridging to core API ───────────────── */

static int
gobject_authorize_cb(const char *client_pubkey_hex,
                     const char *perms_csv,
                     void *user_data)
{
    GNostrNip46Bunker *self = GNOSTR_NIP46_BUNKER(user_data);
    gboolean authorized = FALSE;

    g_signal_emit(self, signals[SIGNAL_AUTHORIZE_REQUEST], 0,
                  client_pubkey_hex, perms_csv, &authorized);

    return authorized ? 1 : 0;
}

static char *
gobject_sign_cb(const char *event_json, void *user_data)
{
    GNostrNip46Bunker *self = GNOSTR_NIP46_BUNKER(user_data);
    gchar *signed_json = NULL;

    g_signal_emit(self, signals[SIGNAL_SIGN_REQUEST], 0,
                  event_json, &signed_json);

    /* Core API expects malloc'd string; convert from g_strdup */
    if (signed_json) {
        char *result = strdup(signed_json);
        g_free(signed_json);
        return result;
    }
    return NULL;
}

/* ── Signal accumulators ───────────────────────────────────────── */

/* For authorize: stop on first TRUE (authorized) */
static gboolean
authorize_accumulator(GSignalInvocationHint *ihint G_GNUC_UNUSED,
                      GValue *return_accu,
                      const GValue *handler_return,
                      gpointer data G_GNUC_UNUSED)
{
    gboolean authorized = g_value_get_boolean(handler_return);
    g_value_set_boolean(return_accu, authorized);
    return !authorized; /* Continue if not yet authorized */
}

/* For sign: stop on first non-NULL result */
static gboolean
sign_accumulator(GSignalInvocationHint *ihint G_GNUC_UNUSED,
                 GValue *return_accu,
                 const GValue *handler_return,
                 gpointer data G_GNUC_UNUSED)
{
    const gchar *result = g_value_get_string(handler_return);
    if (result) {
        g_value_set_string(return_accu, result);
        return FALSE; /* Stop, we have a result */
    }
    return TRUE; /* Continue to next handler */
}

/* ── GObject vfuncs ────────────────────────────────────────────── */

static void
gnostr_nip46_bunker_finalize(GObject *object)
{
    GNostrNip46Bunker *self = GNOSTR_NIP46_BUNKER(object);

    if (self->session) {
        nostr_nip46_session_free(self->session);
        self->session = NULL;
    }

    G_OBJECT_CLASS(gnostr_nip46_bunker_parent_class)->finalize(object);
}

static void
gnostr_nip46_bunker_class_init(GNostrNip46BunkerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = gnostr_nip46_bunker_finalize;

    /**
     * GNostrNip46Bunker::authorize-request:
     * @self: the bunker
     * @client_pubkey_hex: the connecting client's pubkey
     * @perms_csv: (nullable): requested permissions CSV
     *
     * Emitted when a client requests authorization. Handlers should
     * return %TRUE to authorize, %FALSE to deny.
     *
     * Returns: %TRUE if authorized
     */
    signals[SIGNAL_AUTHORIZE_REQUEST] =
        g_signal_new("authorize-request",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     authorize_accumulator, NULL, NULL,
                     G_TYPE_BOOLEAN, 2,
                     G_TYPE_STRING,
                     G_TYPE_STRING);

    /**
     * GNostrNip46Bunker::sign-request:
     * @self: the bunker
     * @event_json: the unsigned event JSON to sign
     *
     * Emitted when a client requests event signing. Handlers should
     * return the signed event JSON string, or %NULL to refuse.
     *
     * Returns: (transfer full) (nullable): signed event JSON
     */
    signals[SIGNAL_SIGN_REQUEST] =
        g_signal_new("sign-request",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     sign_accumulator, NULL, NULL,
                     G_TYPE_STRING, 1,
                     G_TYPE_STRING);

    /**
     * GNostrNip46Bunker::error:
     * @self: the bunker
     * @error: a #GError describing the error
     *
     * Emitted when an error occurs during bunker operations.
     */
    signals[SIGNAL_ERROR] =
        g_signal_new("error",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 1, G_TYPE_ERROR);
}

static void
gnostr_nip46_bunker_init(GNostrNip46Bunker *self)
{
    NostrNip46BunkerCallbacks cbs = {
        .authorize_cb = gobject_authorize_cb,
        .sign_cb = gobject_sign_cb,
        .user_data = self
    };
    self->session = nostr_nip46_bunker_new(&cbs);
}

/* ── Public API ────────────────────────────────────────────────── */

GNostrNip46Bunker *
gnostr_nip46_bunker_new(void)
{
    return g_object_new(GNOSTR_TYPE_NIP46_BUNKER, NULL);
}

gboolean
gnostr_nip46_bunker_listen(GNostrNip46Bunker *self,
                            const gchar *const *relays,
                            gsize n_relays,
                            GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_NIP46_BUNKER(self), FALSE);
    g_return_val_if_fail(self->session != NULL, FALSE);
    g_return_val_if_fail(relays != NULL && n_relays > 0, FALSE);

    int rc = nostr_nip46_bunker_listen(self->session, relays, n_relays);
    if (rc != 0) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_CONNECTION_FAILED,
                            "Failed to start bunker listener");
        return FALSE;
    }

    return TRUE;
}

/* ── Async listen ──────────────────────────────────────────────── */

typedef struct {
    gchar **relays;
    gsize n_relays;
} ListenAsyncData;

static void
listen_async_data_free(gpointer data)
{
    ListenAsyncData *d = data;
    g_strfreev(d->relays);
    g_free(d);
}

static void
listen_thread(GTask        *task,
              gpointer      source_object,
              gpointer      task_data,
              GCancellable *cancellable)
{
    GNostrNip46Bunker *self = GNOSTR_NIP46_BUNKER(source_object);
    ListenAsyncData *d = task_data;
    GError *error = NULL;

    if (g_cancellable_set_error_if_cancelled(cancellable, &error)) {
        g_task_return_error(task, error);
        return;
    }

    if (gnostr_nip46_bunker_listen(self, (const gchar *const *)d->relays,
                                    d->n_relays, &error)) {
        g_task_return_boolean(task, TRUE);
    } else {
        g_task_return_error(task, error);
    }
}

void
gnostr_nip46_bunker_listen_async(GNostrNip46Bunker   *self,
                                  const gchar *const  *relays,
                                  gsize                n_relays,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
    g_return_if_fail(GNOSTR_IS_NIP46_BUNKER(self));
    g_return_if_fail(relays != NULL && n_relays > 0);

    GTask *task = g_task_new(self, cancellable, callback, user_data);
    g_task_set_source_tag(task, gnostr_nip46_bunker_listen_async);

    ListenAsyncData *d = g_new0(ListenAsyncData, 1);
    d->relays = g_strdupv((gchar **)relays);
    d->n_relays = n_relays;
    g_task_set_task_data(task, d, listen_async_data_free);

    g_task_run_in_thread(task, listen_thread);
    g_object_unref(task);
}

gboolean
gnostr_nip46_bunker_listen_finish(GNostrNip46Bunker *self,
                                   GAsyncResult      *result,
                                   GError           **error)
{
    g_return_val_if_fail(GNOSTR_IS_NIP46_BUNKER(self), FALSE);
    g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

    return g_task_propagate_boolean(G_TASK(result), error);
}

gboolean
gnostr_nip46_bunker_issue_uri(GNostrNip46Bunker *self,
                               const gchar *signer_pubkey_hex,
                               const gchar *const *relays,
                               gsize n_relays,
                               const gchar *secret,
                               gchar **out_uri,
                               GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_NIP46_BUNKER(self), FALSE);
    g_return_val_if_fail(self->session != NULL, FALSE);
    g_return_val_if_fail(signer_pubkey_hex != NULL, FALSE);
    g_return_val_if_fail(out_uri != NULL, FALSE);

    char *uri = NULL;
    int rc = nostr_nip46_bunker_issue_bunker_uri(self->session,
                                                  signer_pubkey_hex,
                                                  relays, n_relays,
                                                  secret, &uri);
    if (rc != 0) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_KEY,
                            "Failed to generate bunker URI");
        return FALSE;
    }

    *out_uri = g_strdup(uri);
    free(uri);
    return TRUE;
}

gboolean
gnostr_nip46_bunker_handle_cipher(GNostrNip46Bunker *self,
                                   const gchar *client_pubkey_hex,
                                   const gchar *ciphertext,
                                   gchar **out_cipher_reply,
                                   GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_NIP46_BUNKER(self), FALSE);
    g_return_val_if_fail(self->session != NULL, FALSE);
    g_return_val_if_fail(client_pubkey_hex != NULL, FALSE);
    g_return_val_if_fail(ciphertext != NULL, FALSE);
    g_return_val_if_fail(out_cipher_reply != NULL, FALSE);

    char *reply = NULL;
    int rc = nostr_nip46_bunker_handle_cipher(self->session,
                                               client_pubkey_hex,
                                               ciphertext,
                                               &reply);
    if (rc != 0) {
        g_set_error_literal(error, NOSTR_ERROR, NOSTR_ERROR_ENCRYPTION_FAILED,
                            "Failed to handle cipher request");
        return FALSE;
    }

    *out_cipher_reply = g_strdup(reply);
    free(reply);
    return TRUE;
}

NostrNip46Session *
gnostr_nip46_bunker_get_session(GNostrNip46Bunker *self)
{
    g_return_val_if_fail(GNOSTR_IS_NIP46_BUNKER(self), NULL);
    return self->session;
}
