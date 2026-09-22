/* nostrc-8ya7: GLib async wrappers for NIP-46 client RPC.
 *
 * Provides GAsyncReadyCallback-based async API for NIP-46 operations,
 * integrating with GMainContext for safe GTK thread marshaling.
 *
 * Pattern: GTask + g_task_run_in_thread wrapping sync nip46_rpc_call.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "nostr/nip46/nip46_client_g.h"
#include "nostr/nip46/nip46_client.h"
#include <gio/gio.h>
#include <string.h>

/* Task data for sign_event async */
typedef struct {
    NostrNip46Session *session;
    char *event_json;
} SignEventData;

static void sign_event_data_free(gpointer data) {
    SignEventData *d = data;
    if (d) {
        nostr_nip46_session_unref(d->session);
        g_free(d->event_json);
        g_free(d);
    }
}

/* C2 (nostrc-ot2c.3): translate a fired GCancellable into a cancel-handle
 * signal so the sync RPC's response wait is interrupted mid-flight instead
 * of only being observed after the RPC returns. Called from the same thread
 * that triggered g_cancellable_cancel(). */
static void nip46_g_cancel_relay(GCancellable *cancellable, gpointer user_data) {
    (void)cancellable;
    NostrNip46CancelHandle *h = user_data;
    nostr_nip46_cancel_handle_cancel(h);
}

static void
sign_event_thread(GTask        *task,
                  gpointer      source_object,
                  gpointer      task_data,
                  GCancellable *cancellable)
{
    (void)source_object;
    SignEventData *d = task_data;

    if (g_cancellable_is_cancelled(cancellable)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "Sign event cancelled");
        return;
    }

    NostrNip46CancelHandle *handle = nostr_nip46_cancel_handle_new();
    gulong hook = 0;
    if (cancellable && handle) {
        hook = g_cancellable_connect(cancellable,
            G_CALLBACK(nip46_g_cancel_relay), handle, NULL);
    }
    NostrNip46RequestOptions opts = { .deadline_ms = 0, .cancel_handle = handle };

    char *signed_event = NULL;
    int rc = nostr_nip46_client_sign_event_opts(d->session, d->event_json, &opts, &signed_event);

    if (cancellable && hook) g_cancellable_disconnect(cancellable, hook);
    if (handle) nostr_nip46_cancel_handle_unref(handle);

    if (g_cancellable_is_cancelled(cancellable)) {
        free(signed_event);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "Sign event cancelled");
        return;
    }

    if (rc == 0 && signed_event) {
        g_task_return_pointer(task, signed_event, free);
    } else {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "NIP-46 sign_event RPC failed");
    }
}

void
nostr_nip46_client_sign_event_g_async(NostrNip46Session   *session,
                                       const char          *event_json,
                                       GCancellable        *cancellable,
                                       GAsyncReadyCallback  callback,
                                       gpointer             user_data)
{
    g_return_if_fail(session != NULL);
    g_return_if_fail(event_json != NULL);

    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_set_source_tag(task, nostr_nip46_client_sign_event_g_async);

    SignEventData *d = g_new0(SignEventData, 1);
    d->session = nostr_nip46_session_ref(session);
    d->event_json = g_strdup(event_json);
    g_task_set_task_data(task, d, sign_event_data_free);

    g_task_run_in_thread(task, sign_event_thread);
    g_object_unref(task);
}

char *
nostr_nip46_client_sign_event_g_finish(GAsyncResult  *result,
                                        GError       **error)
{
    g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
    return g_task_propagate_pointer(G_TASK(result), error);
}

/* Task data for connect_rpc async */
typedef struct {
    NostrNip46Session *session;
    char *connect_secret;
    char *perms;
} ConnectRpcData;

static void connect_rpc_data_free(gpointer data) {
    ConnectRpcData *d = data;
    if (d) {
        nostr_nip46_session_unref(d->session);
        if (d->connect_secret)
            memset(d->connect_secret, 0, strlen(d->connect_secret));
        g_free(d->connect_secret);
        g_free(d->perms);
        g_free(d);
    }
}

static void
connect_rpc_thread(GTask        *task,
                   gpointer      source_object,
                   gpointer      task_data,
                   GCancellable *cancellable)
{
    (void)source_object;
    ConnectRpcData *d = task_data;

    if (g_cancellable_is_cancelled(cancellable)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "Connect RPC cancelled");
        return;
    }

    NostrNip46CancelHandle *handle = nostr_nip46_cancel_handle_new();
    gulong hook = 0;
    if (cancellable && handle) {
        hook = g_cancellable_connect(cancellable,
            G_CALLBACK(nip46_g_cancel_relay), handle, NULL);
    }
    NostrNip46RequestOptions opts = { .deadline_ms = 0, .cancel_handle = handle };

    char *result = NULL;
    int rc = nostr_nip46_client_connect_rpc_opts(d->session, d->connect_secret,
                                                  d->perms, &opts, &result);

    if (cancellable && hook) g_cancellable_disconnect(cancellable, hook);
    if (handle) nostr_nip46_cancel_handle_unref(handle);

    if (g_cancellable_is_cancelled(cancellable)) {
        free(result);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "Connect RPC cancelled");
        return;
    }

    if (rc == 0 && result) {
        g_task_return_pointer(task, result, free);
    } else {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "NIP-46 connect RPC failed");
    }
}

void
nostr_nip46_client_connect_rpc_g_async(NostrNip46Session   *session,
                                        const char          *connect_secret,
                                        const char          *perms,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data)
{
    g_return_if_fail(session != NULL);

    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_set_source_tag(task, nostr_nip46_client_connect_rpc_g_async);

    ConnectRpcData *d = g_new0(ConnectRpcData, 1);
    d->session = nostr_nip46_session_ref(session);
    d->connect_secret = g_strdup(connect_secret);
    d->perms = g_strdup(perms);
    g_task_set_task_data(task, d, connect_rpc_data_free);

    g_task_run_in_thread(task, connect_rpc_thread);
    g_object_unref(task);
}

char *
nostr_nip46_client_connect_rpc_g_finish(GAsyncResult  *result,
                                         GError       **error)
{
    g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
    return g_task_propagate_pointer(G_TASK(result), error);
}

/* Task data for get_public_key_rpc async */
typedef struct {
    NostrNip46Session *session;
} GetPubkeyData;

static void get_pubkey_data_free(gpointer data) {
    GetPubkeyData *d = data;
    if (d) nostr_nip46_session_unref(d->session);
    g_free(d);
}

static void
get_pubkey_rpc_thread(GTask        *task,
                      gpointer      source_object,
                      gpointer      task_data,
                      GCancellable *cancellable)
{
    (void)source_object;
    GetPubkeyData *d = task_data;

    if (g_cancellable_is_cancelled(cancellable)) {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "Get public key cancelled");
        return;
    }

    NostrNip46CancelHandle *handle = nostr_nip46_cancel_handle_new();
    gulong hook = 0;
    if (cancellable && handle) {
        hook = g_cancellable_connect(cancellable,
            G_CALLBACK(nip46_g_cancel_relay), handle, NULL);
    }
    NostrNip46RequestOptions opts = { .deadline_ms = 0, .cancel_handle = handle };

    char *pubkey = NULL;
    int rc = nostr_nip46_client_get_public_key_rpc_opts(d->session, &opts, &pubkey);

    if (cancellable && hook) g_cancellable_disconnect(cancellable, hook);
    if (handle) nostr_nip46_cancel_handle_unref(handle);

    if (g_cancellable_is_cancelled(cancellable)) {
        free(pubkey);
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                "Get public key cancelled");
        return;
    }

    if (rc == 0 && pubkey) {
        g_task_return_pointer(task, pubkey, free);
    } else {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "NIP-46 get_public_key RPC failed");
    }
}

void
nostr_nip46_client_get_public_key_rpc_g_async(NostrNip46Session   *session,
                                               GCancellable        *cancellable,
                                               GAsyncReadyCallback  callback,
                                               gpointer             user_data)
{
    g_return_if_fail(session != NULL);

    GTask *task = g_task_new(NULL, cancellable, callback, user_data);
    g_task_set_source_tag(task, nostr_nip46_client_get_public_key_rpc_g_async);

    GetPubkeyData *d = g_new0(GetPubkeyData, 1);
    d->session = nostr_nip46_session_ref(session);
    g_task_set_task_data(task, d, get_pubkey_data_free);

    g_task_run_in_thread(task, get_pubkey_rpc_thread);
    g_object_unref(task);
}

char *
nostr_nip46_client_get_public_key_rpc_g_finish(GAsyncResult  *result,
                                                GError       **error)
{
    g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
    return g_task_propagate_pointer(G_TASK(result), error);
}
