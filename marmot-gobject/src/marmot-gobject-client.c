/*
 * marmot-gobject - MarmotGobjectClient implementation
 *
 * The main Marmot protocol client wrapped as a GObject with GTask-based
 * async operations. All crypto/MLS operations run in a thread pool via
 * g_task_run_in_thread() to avoid blocking the GTK main loop.
 *
 * SPDX-License-Identifier: MIT
 */

#include "marmot-gobject-1.0/marmot-gobject-client.h"
#include "marmot-gobject-1.0/marmot-gobject-storage.h"
#include <marmot/marmot.h>
#include "marmot-gobject-1.0/marmot-gobject-key-package.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Error domain ────────────────────────────────────────────────── */

#define MARMOT_GOBJECT_ERROR (marmot_gobject_client_error_quark())

typedef enum {
    MARMOT_GOBJECT_ERROR_INVALID_INPUT = 1,
    MARMOT_GOBJECT_ERROR_INVALID_HEX,
    MARMOT_GOBJECT_ERROR_STORAGE,
    MARMOT_GOBJECT_ERROR_MARMOT,
} MarmotGobjectErrorCode;

static GQuark
marmot_gobject_client_error_quark(void)
{
    return g_quark_from_static_string("marmot-gobject-client-error");
}

/* ── Signals ─────────────────────────────────────────────────────── */

enum {
    SIGNAL_GROUP_JOINED,
    SIGNAL_MESSAGE_RECEIVED,
    SIGNAL_WELCOME_RECEIVED,
    SIGNAL_GROUP_UPDATED,
    N_SIGNALS
};

static guint client_signals[N_SIGNALS] = { 0 };

/* ── Instance ────────────────────────────────────────────────────── */

struct _MarmotGobjectClient {
    GObject parent_instance;
    Marmot *marmot;
    MarmotGobjectStorage *storage;  /* strong ref */
    /* libmarmot and its storage backends have no locks, and every async
     * call runs on the shared GTask thread pool: this serializes all
     * libmarmot calls of the client, async and sync (W17b addendum C1). */
    GMutex lock;
};

G_DEFINE_TYPE(MarmotGobjectClient, marmot_gobject_client, G_TYPE_OBJECT)

static void
marmot_gobject_client_finalize(GObject *object)
{
    MarmotGobjectClient *self = MARMOT_GOBJECT_CLIENT(object);

    if (self->marmot) {
        marmot_free(self->marmot);
        self->marmot = NULL;
    }
    g_clear_object(&self->storage);

    g_mutex_clear(&self->lock);
    G_OBJECT_CLASS(marmot_gobject_client_parent_class)->finalize(object);
}

static void
marmot_gobject_client_class_init(MarmotGobjectClientClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS(klass);
    oc->finalize = marmot_gobject_client_finalize;

    /**
     * MarmotGobjectClient::group-joined:
     * @client: the client
     * @group: (transfer none): the joined #MarmotGobjectGroup
     *
     * Emitted when a group is joined via welcome acceptance.
     */
    client_signals[SIGNAL_GROUP_JOINED] =
        g_signal_new("group-joined",
                      G_TYPE_FROM_CLASS(klass),
                      G_SIGNAL_RUN_LAST, 0,
                      NULL, NULL, NULL,
                      G_TYPE_NONE, 1,
                      MARMOT_GOBJECT_TYPE_GROUP);

    /**
     * MarmotGobjectClient::message-received:
     * @client: the client
     * @message: (transfer none): the received #MarmotGobjectMessage
     *
     * Emitted when a group message is decrypted.
     */
    client_signals[SIGNAL_MESSAGE_RECEIVED] =
        g_signal_new("message-received",
                      G_TYPE_FROM_CLASS(klass),
                      G_SIGNAL_RUN_LAST, 0,
                      NULL, NULL, NULL,
                      G_TYPE_NONE, 1,
                      MARMOT_GOBJECT_TYPE_MESSAGE);

    /**
     * MarmotGobjectClient::welcome-received:
     * @client: the client
     * @welcome: (transfer none): the received #MarmotGobjectWelcome
     *
     * Emitted when a welcome message is processed.
     */
    client_signals[SIGNAL_WELCOME_RECEIVED] =
        g_signal_new("welcome-received",
                      G_TYPE_FROM_CLASS(klass),
                      G_SIGNAL_RUN_LAST, 0,
                      NULL, NULL, NULL,
                      G_TYPE_NONE, 1,
                      MARMOT_GOBJECT_TYPE_WELCOME);

    /**
     * MarmotGobjectClient::group-updated:
     * @client: the client
     * @group: (transfer none): the group as stored after the change
     *
     * Emitted on the main context when a group moved to a new epoch: a
     * received Commit was applied (marmot_gobject_client_process_message_async()
     * finished with %MARMOT_GOBJECT_MESSAGE_RESULT_COMMIT) or a local
     * metadata update was committed.
     *
     * Since: 1.2
     */
    client_signals[SIGNAL_GROUP_UPDATED] =
        g_signal_new("group-updated",
                      G_TYPE_FROM_CLASS(klass),
                      G_SIGNAL_RUN_LAST, 0,
                      NULL, NULL, NULL,
                      G_TYPE_NONE, 1,
                      MARMOT_GOBJECT_TYPE_GROUP);
}

static void
marmot_gobject_client_init(MarmotGobjectClient *self)
{
    g_mutex_init(&self->lock);
}

MarmotGobjectClient *
marmot_gobject_client_new(MarmotGobjectStorage *storage)
{
    g_return_val_if_fail(MARMOT_GOBJECT_IS_STORAGE(storage), NULL);

    MarmotStorage *raw = marmot_gobject_storage_steal_raw_storage(storage);
    g_return_val_if_fail(raw != NULL, NULL);

    /* marmot_new() takes ownership of the storage pointer. */
    Marmot *m = marmot_new(raw);
    if (!m) {
        marmot_storage_free(raw);
        return NULL;
    }

    MarmotGobjectClient *self = g_object_new(MARMOT_GOBJECT_TYPE_CLIENT, NULL);
    self->marmot = m;
    self->storage = g_object_ref(storage);
    return self;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Helpers: hex conversion
 * ══════════════════════════════════════════════════════════════════════════ */

static gboolean
hex_to_bytes(const gchar *hex, uint8_t *out, size_t expected_len)
{
    if (!hex || strlen(hex) != expected_len * 2)
        return FALSE;

    for (size_t i = 0; i < expected_len; i++) {
        guint byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1)
            return FALSE;
        if (byte > 0xFF)  /* Validate byte range */
            return FALSE;
        out[i] = (uint8_t)byte;
    }
    return TRUE;
}

static gchar *
bytes_to_hex(const uint8_t *data, size_t len)
{
    gchar *hex = g_malloc(len * 2 + 1);
    for (size_t i = 0; i < len; i++)
        snprintf(hex + i * 2, 3, "%02x", data[i]);
    hex[len * 2] = '\0';
    return hex;
}

static gchar **
admin_pubkeys_to_hex_strv(const uint8_t (*admin_pubkeys)[32], size_t admin_count)
{
    if (!admin_pubkeys || admin_count == 0)
        return NULL;

    gchar **hexes = g_new0(gchar *, admin_count + 1);
    for (size_t i = 0; i < admin_count; i++)
        hexes[i] = bytes_to_hex(admin_pubkeys[i], 32);
    return hexes;
}

static gchar **
welcome_relays_to_strv(char **relays, size_t relay_count)
{
    if (!relays || relay_count == 0)
        return NULL;

    gchar **urls = g_new0(gchar *, relay_count + 1);
    for (size_t i = 0; i < relay_count; i++)
        urls[i] = g_strdup(relays[i]);
    return urls;
}

static MarmotGobjectGroup *
gobject_group_from_marmot(const MarmotGroup *group)
{
    if (!group)
        return NULL;

    gchar *mls_hex = marmot_group_id_to_hex(&group->mls_group_id);
    gchar *nostr_hex = bytes_to_hex(group->nostr_group_id, 32);
    gchar **admins = admin_pubkeys_to_hex_strv(
        (const uint8_t (*)[32])group->admin_pubkeys, group->admin_count);

    MarmotGobjectGroup *gobj = marmot_gobject_group_new_from_data_full(
        mls_hex, nostr_hex,
        group->name, group->description,
        marmot_gobject_group_state_from_marmot(group->state),
        group->epoch,
        (const gchar * const *)admins,
        (guint)group->admin_count,
        group->last_message_at);

    g_free(mls_hex);
    g_free(nostr_hex);
    g_strfreev(admins);
    return gobj;
}

static MarmotGobjectMessage *
gobject_message_from_marmot(const MarmotMessage *msg)
{
    if (!msg)
        return NULL;

    gchar *evt_hex = bytes_to_hex(msg->id, 32);
    gchar *pk_hex = bytes_to_hex(msg->pubkey, 32);
    gchar *grp_hex = marmot_group_id_to_hex(&msg->mls_group_id);

    MarmotGobjectMessage *gobj = marmot_gobject_message_new_from_data_full(
        evt_hex, pk_hex, msg->content, msg->kind,
        msg->created_at, msg->processed_at, grp_hex,
        msg->epoch,
        marmot_gobject_message_state_from_marmot(msg->state),
        msg->event_json);

    g_free(evt_hex);
    g_free(pk_hex);
    g_free(grp_hex);
    return gobj;
}

static MarmotGobjectWelcome *
gobject_welcome_from_marmot(const MarmotWelcome *welcome)
{
    if (!welcome)
        return NULL;

    gchar *mls_hex = marmot_group_id_to_hex(&welcome->mls_group_id);
    gchar *nostr_hex = bytes_to_hex(welcome->nostr_group_id, 32);
    gchar *welc_hex = bytes_to_hex(welcome->welcomer, 32);
    gchar *evt_hex = bytes_to_hex(welcome->id, 32);
    gchar *wrapper_hex = bytes_to_hex(welcome->wrapper_event_id, 32);
    gchar **relays = welcome_relays_to_strv(welcome->group_relays,
                                            welcome->group_relay_count);

    MarmotGobjectWelcome *gobj = marmot_gobject_welcome_new_from_data_full(
        evt_hex, welcome->group_name, welcome->group_description,
        welc_hex, welcome->member_count,
        marmot_gobject_welcome_state_from_marmot(welcome->state),
        mls_hex, nostr_hex, wrapper_hex,
        (const gchar * const *)relays);

    g_free(mls_hex);
    g_free(nostr_hex);
    g_free(welc_hex);
    g_free(evt_hex);
    g_free(wrapper_hex);
    g_strfreev(relays);
    return gobj;
}

typedef struct {
    MarmotGobjectClient *client;
    guint signal_id;
    GObject *object;
} QueuedSignal;

static gboolean
emit_queued_signal(gpointer user_data)
{
    QueuedSignal *queued = user_data;
    g_signal_emit(queued->client, queued->signal_id, 0, queued->object);
    g_object_unref(queued->object);
    g_object_unref(queued->client);
    g_free(queued);
    return G_SOURCE_REMOVE;
}

static void
queue_client_signal(MarmotGobjectClient *self, guint signal_id, GObject *object)
{
    if (!object)
        return;

    QueuedSignal *queued = g_new0(QueuedSignal, 1);
    queued->client = g_object_ref(self);
    queued->signal_id = signal_id;
    queued->object = g_object_ref(object);
    g_main_context_invoke(NULL, emit_queued_signal, queued);
}

static MarmotGobjectMessage *
find_processed_message_for_inner_json(Marmot *marmot, const gchar *inner_json)
{
    if (!marmot || !inner_json)
        return NULL;

    MarmotGroup **groups = NULL;
    size_t group_count = 0;
    if (marmot_get_all_groups(marmot, &groups, &group_count) != MARMOT_OK)
        return NULL;

    MarmotGobjectMessage *found = NULL;
    for (size_t i = 0; i < group_count && !found; i++) {
        MarmotPagination page = marmot_pagination_default();
        MarmotMessage **messages = NULL;
        size_t message_count = 0;
        if (marmot_get_messages(marmot, &groups[i]->mls_group_id, &page,
                                &messages, &message_count) == MARMOT_OK) {
            for (size_t j = 0; j < message_count; j++) {
                if (!found && messages[j]->state == MARMOT_MSG_STATE_PROCESSED &&
                    messages[j]->content && strcmp(messages[j]->content, inner_json) == 0) {
                    found = gobject_message_from_marmot(messages[j]);
                }
                marmot_message_free(messages[j]);
            }
            free(messages);
        }
        marmot_group_free(groups[i]);
    }
    free(groups);
    return found;
}

static void
set_marmot_error(GError **error, MarmotError err)
{
    g_set_error(error, MARMOT_GOBJECT_ERROR, (gint)err,
                "%s", marmot_error_string(err));
}

/* ══════════════════════════════════════════════════════════════════════════
 * MIP-00: Key Package (async via GTask)
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    gchar *pubkey_hex;
    gchar *sk_hex;
    gchar **relay_urls;
} CreateKeyPackageData;

static void
create_key_package_data_free(gpointer data)
{
    CreateKeyPackageData *d = data;
    g_free(d->pubkey_hex);
    g_free(d->sk_hex);
    g_strfreev(d->relay_urls);
    g_free(d);
}

static void
create_key_package_thread(GTask *task, gpointer source_object,
                           gpointer task_data, GCancellable *cancellable)
{
    MarmotGobjectClient *self = MARMOT_GOBJECT_CLIENT(source_object);
    /* One libmarmot call at a time per client (a Marmot is not thread-safe). */
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->lock);
    CreateKeyPackageData *d = task_data;
    (void)cancellable;

    uint8_t pubkey[32], sk[32];
    if (!hex_to_bytes(d->pubkey_hex, pubkey, 32) ||
        !hex_to_bytes(d->sk_hex, sk, 32)) {
        g_task_return_new_error(task, MARMOT_GOBJECT_ERROR,
                                MARMOT_GOBJECT_ERROR_INVALID_HEX,
                                "Invalid hex pubkey or secret key");
        return;
    }

    size_t relay_count = 0;
    if (d->relay_urls) {
        while (d->relay_urls[relay_count] && relay_count < 1000) relay_count++;
    }

    MarmotKeyPackageResult result = { 0 };
    MarmotError err = marmot_create_key_package(
        self->marmot, pubkey, sk,
        (const char **)d->relay_urls, relay_count, &result);

    if (err != MARMOT_OK) {
        g_task_return_new_error(task, MARMOT_GOBJECT_ERROR, (gint)err,
                                "%s", marmot_error_string(err));
        return;
    }

    g_task_return_pointer(task, g_strdup(result.event_json), g_free);
    marmot_key_package_result_free(&result);
}

void
marmot_gobject_client_create_key_package_async(MarmotGobjectClient *self,
                                                 const gchar *nostr_pubkey_hex,
                                                 const gchar *nostr_sk_hex,
                                                 const gchar * const *relay_urls,
                                                 GCancellable *cancellable,
                                                 GAsyncReadyCallback callback,
                                                 gpointer user_data)
{
    g_return_if_fail(MARMOT_GOBJECT_IS_CLIENT(self));
    g_return_if_fail(nostr_pubkey_hex != NULL);
    g_return_if_fail(nostr_sk_hex != NULL);

    GTask *task = g_task_new(self, cancellable, callback, user_data);

    CreateKeyPackageData *d = g_new0(CreateKeyPackageData, 1);
    d->pubkey_hex = g_strdup(nostr_pubkey_hex);
    d->sk_hex     = g_strdup(nostr_sk_hex);
    d->relay_urls = g_strdupv((gchar **)relay_urls);
    g_task_set_task_data(task, d, create_key_package_data_free);

    g_task_run_in_thread(task, create_key_package_thread);
    g_object_unref(task);
}

gchar *
marmot_gobject_client_create_key_package_finish(MarmotGobjectClient *self,
                                                  GAsyncResult *result,
                                                  GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, self), NULL);
    return g_task_propagate_pointer(G_TASK(result), error);
}

/* ── Unsigned key package (signer-only flow) ─────────────────────── */

typedef struct {
    gchar *pubkey_hex;
    gchar **relay_urls;
} CreateKeyPackageUnsignedData;

static void
create_key_package_unsigned_data_free(gpointer data)
{
    CreateKeyPackageUnsignedData *d = data;
    g_free(d->pubkey_hex);
    g_strfreev(d->relay_urls);
    g_free(d);
}

static void
create_key_package_unsigned_thread(GTask *task, gpointer source_object,
                                    gpointer task_data, GCancellable *cancellable)
{
    MarmotGobjectClient *self = MARMOT_GOBJECT_CLIENT(source_object);
    /* One libmarmot call at a time per client (a Marmot is not thread-safe). */
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->lock);
    CreateKeyPackageUnsignedData *d = task_data;
    (void)cancellable;

    uint8_t pubkey[32];
    if (!hex_to_bytes(d->pubkey_hex, pubkey, 32)) {
        g_task_return_new_error(task, MARMOT_GOBJECT_ERROR,
                                MARMOT_GOBJECT_ERROR_INVALID_HEX,
                                "Invalid hex pubkey");
        return;
    }

    size_t relay_count = 0;
    if (d->relay_urls) {
        while (d->relay_urls[relay_count] && relay_count < 1000) relay_count++;
    }

    MarmotKeyPackageResult result = { 0 };
    MarmotError err = marmot_create_key_package_unsigned(
        self->marmot, pubkey,
        (const char **)d->relay_urls, relay_count, &result);

    if (err != MARMOT_OK) {
        g_task_return_new_error(task, MARMOT_GOBJECT_ERROR, (gint)err,
                                "%s", marmot_error_string(err));
        return;
    }

    g_task_return_pointer(task, g_strdup(result.event_json), g_free);
    marmot_key_package_result_free(&result);
}

void
marmot_gobject_client_create_key_package_unsigned_async(MarmotGobjectClient *self,
                                                          const gchar *nostr_pubkey_hex,
                                                          const gchar * const *relay_urls,
                                                          GCancellable *cancellable,
                                                          GAsyncReadyCallback callback,
                                                          gpointer user_data)
{
    g_return_if_fail(MARMOT_GOBJECT_IS_CLIENT(self));
    g_return_if_fail(nostr_pubkey_hex != NULL);

    GTask *task = g_task_new(self, cancellable, callback, user_data);

    CreateKeyPackageUnsignedData *d = g_new0(CreateKeyPackageUnsignedData, 1);
    d->pubkey_hex = g_strdup(nostr_pubkey_hex);
    d->relay_urls = g_strdupv((gchar **)relay_urls);
    g_task_set_task_data(task, d, create_key_package_unsigned_data_free);

    g_task_run_in_thread(task, create_key_package_unsigned_thread);
    g_object_unref(task);
}

gchar *
marmot_gobject_client_create_key_package_unsigned_finish(MarmotGobjectClient *self,
                                                           GAsyncResult *result,
                                                           GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, self), NULL);
    return g_task_propagate_pointer(G_TASK(result), error);
}

/* ── KeyPackage selection (kind:30443 addressable slots) ─────────── */

gint
marmot_gobject_select_key_package_event(const gchar * const *event_jsons,
                                        const gchar *owner_pubkey_hex,
                                        GError **error)
{
    g_return_val_if_fail(error == NULL || *error == NULL, -1);

    uint8_t owner[32];
    if (owner_pubkey_hex && !hex_to_bytes(owner_pubkey_hex, owner, sizeof(owner))) {
        g_set_error(error, MARMOT_GOBJECT_ERROR, MARMOT_GOBJECT_ERROR_INVALID_HEX,
                    "Invalid hex owner pubkey");
        return -1;
    }

    size_t count = event_jsons ? g_strv_length((gchar **)event_jsons) : 0;
    if (count > (size_t)G_MAXINT) {
        g_set_error(error, MARMOT_GOBJECT_ERROR, MARMOT_GOBJECT_ERROR_INVALID_INPUT,
                    "Too many KeyPackage events");
        return -1;
    }

    size_t index = 0;
    MarmotError err = marmot_select_key_package_event((const char **)event_jsons, count,
                                                      owner_pubkey_hex ? owner : NULL,
                                                      &index);
    if (err != MARMOT_OK) {
        set_marmot_error(error, err);
        return -1;
    }
    return (gint)index;
}

/* ══════════════════════════════════════════════════════════════════════════
 * MIP-01: Group Creation (async)
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    gchar *creator_pubkey_hex;
    gchar **key_package_jsons;
    gchar *group_name;
    gchar *group_description;
    gchar **admin_pubkey_hexes;
    gchar **relay_urls;
    /* Outputs */
    gchar **welcome_jsons;
    gchar *evolution_json;
} CreateGroupData;

static void
create_group_data_free(gpointer data)
{
    CreateGroupData *d = data;
    g_free(d->creator_pubkey_hex);
    g_strfreev(d->key_package_jsons);
    g_free(d->group_name);
    g_free(d->group_description);
    g_strfreev(d->admin_pubkey_hexes);
    g_strfreev(d->relay_urls);
    g_strfreev(d->welcome_jsons);
    g_free(d->evolution_json);
    g_free(d);
}

static void
create_group_thread(GTask *task, gpointer source_object,
                     gpointer task_data, GCancellable *cancellable)
{
    MarmotGobjectClient *self = MARMOT_GOBJECT_CLIENT(source_object);
    /* One libmarmot call at a time per client (a Marmot is not thread-safe). */
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->lock);
    CreateGroupData *d = task_data;
    (void)cancellable;

    uint8_t creator_pk[32];
    if (!hex_to_bytes(d->creator_pubkey_hex, creator_pk, 32)) {
        g_task_return_new_error(task, MARMOT_GOBJECT_ERROR,
                                MARMOT_GOBJECT_ERROR_INVALID_HEX,
                                "Invalid creator pubkey hex");
        return;
    }

    size_t kp_count = 0;
    if (d->key_package_jsons) {
        while (d->key_package_jsons[kp_count] && kp_count < 1000) kp_count++;
    }

    /* Build MarmotGroupConfig */
    MarmotGroupConfig config = { 0 };
    config.name = d->group_name;
    config.description = d->group_description;

    size_t relay_count = 0;
    if (d->relay_urls) {
        while (d->relay_urls[relay_count] && relay_count < 1000) relay_count++;
        config.relay_urls = d->relay_urls;
        config.relay_count = relay_count;
    }

    /* Parse admin pubkeys */
    size_t admin_count = 0;
    uint8_t (*admin_pks)[32] = NULL;
    if (d->admin_pubkey_hexes) {
        while (d->admin_pubkey_hexes[admin_count] && admin_count < 1000) admin_count++;
        if (admin_count > 0) {
            admin_pks = (uint8_t (*)[32])g_malloc0(admin_count * 32);
            for (size_t i = 0; i < admin_count; i++) {
                if (!hex_to_bytes(d->admin_pubkey_hexes[i], admin_pks[i], 32)) {
                    g_free(admin_pks);  /* Fix memory leak */
                    g_task_return_new_error(task, MARMOT_GOBJECT_ERROR,
                                            MARMOT_GOBJECT_ERROR_INVALID_HEX,
                                            "Invalid admin pubkey hex");
                    return;
                }
            }
            config.admin_pubkeys = admin_pks;
            config.admin_count = admin_count;
        }
    }

    MarmotCreateGroupResult result = { 0 };
    MarmotError err = marmot_create_group(
        self->marmot, creator_pk,
        (const char **)d->key_package_jsons, kp_count,
        &config, &result);

    g_free(admin_pks);

    if (err != MARMOT_OK) {
        g_task_return_new_error(task, MARMOT_GOBJECT_ERROR, (gint)err,
                                "%s", marmot_error_string(err));
        return;
    }

    /* Store output data for the finish function */
    if (result.welcome_count > 0) {
        d->welcome_jsons = g_new0(gchar *, result.welcome_count + 1);
        for (size_t i = 0; i < result.welcome_count; i++)
            d->welcome_jsons[i] = g_strdup(result.welcome_rumor_jsons[i]);
    }
    d->evolution_json = g_strdup(result.evolution_event_json);

    /* Build the GObject group */
    MarmotGobjectGroup *group = gobject_group_from_marmot(result.group);

    marmot_create_group_result_free(&result);
    g_task_return_pointer(task, group, g_object_unref);
}

void
marmot_gobject_client_create_group_async(MarmotGobjectClient *self,
                                           const gchar *creator_pubkey_hex,
                                           const gchar * const *key_package_jsons,
                                           const gchar *group_name,
                                           const gchar *group_description,
                                           const gchar * const *admin_pubkey_hexes,
                                           const gchar * const *relay_urls,
                                           GCancellable *cancellable,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data)
{
    g_return_if_fail(MARMOT_GOBJECT_IS_CLIENT(self));
    g_return_if_fail(creator_pubkey_hex != NULL);
    g_return_if_fail(key_package_jsons != NULL);

    GTask *task = g_task_new(self, cancellable, callback, user_data);

    CreateGroupData *d = g_new0(CreateGroupData, 1);
    d->creator_pubkey_hex = g_strdup(creator_pubkey_hex);
    d->key_package_jsons  = g_strdupv((gchar **)key_package_jsons);
    d->group_name         = g_strdup(group_name);
    d->group_description  = g_strdup(group_description);
    d->admin_pubkey_hexes = g_strdupv((gchar **)admin_pubkey_hexes);
    d->relay_urls         = g_strdupv((gchar **)relay_urls);
    g_task_set_task_data(task, d, create_group_data_free);

    g_task_run_in_thread(task, create_group_thread);
    g_object_unref(task);
}

MarmotGobjectGroup *
marmot_gobject_client_create_group_finish(MarmotGobjectClient *self,
                                           GAsyncResult *result,
                                           gchar ***out_welcome_jsons,
                                           gchar **out_evolution_json,
                                           GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, self), NULL);

    GTask *task = G_TASK(result);
    CreateGroupData *d = g_task_get_task_data(task);

    MarmotGobjectGroup *group = g_task_propagate_pointer(task, error);
    if (!group)
        return NULL;

    if (out_welcome_jsons) {
        *out_welcome_jsons = d->welcome_jsons;
        d->welcome_jsons = NULL;  /* transfer ownership */
    }
    if (out_evolution_json) {
        *out_evolution_json = d->evolution_json;
        d->evolution_json = NULL;
    }

    return group;
}

/* ══════════════════════════════════════════════════════════════════════════
 * MIP-02: Welcome Processing (async)
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    gchar *wrapper_event_id_hex;
    gchar *rumor_event_json;
} ProcessWelcomeData;

static void
process_welcome_data_free(gpointer data)
{
    ProcessWelcomeData *d = data;
    g_free(d->wrapper_event_id_hex);
    g_free(d->rumor_event_json);
    g_free(d);
}

static void
process_welcome_thread(GTask *task, gpointer source_object,
                        gpointer task_data, GCancellable *cancellable)
{
    MarmotGobjectClient *self = MARMOT_GOBJECT_CLIENT(source_object);
    /* One libmarmot call at a time per client (a Marmot is not thread-safe). */
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->lock);
    ProcessWelcomeData *d = task_data;
    (void)cancellable;

    uint8_t wrapper_id[32];
    if (!hex_to_bytes(d->wrapper_event_id_hex, wrapper_id, 32)) {
        g_task_return_new_error(task, MARMOT_GOBJECT_ERROR,
                                MARMOT_GOBJECT_ERROR_INVALID_HEX,
                                "Invalid wrapper event ID hex");
        return;
    }

    MarmotWelcome *welcome = NULL;
    MarmotError err = marmot_process_welcome(
        self->marmot, wrapper_id, d->rumor_event_json, &welcome);

    if (err != MARMOT_OK) {
        g_task_return_new_error(task, MARMOT_GOBJECT_ERROR, (gint)err,
                                "%s", marmot_error_string(err));
        return;
    }

    MarmotGobjectWelcome *gobj = gobject_welcome_from_marmot(welcome);
    marmot_welcome_free(welcome);

    queue_client_signal(self, client_signals[SIGNAL_WELCOME_RECEIVED], G_OBJECT(gobj));
    g_task_return_pointer(task, gobj, g_object_unref);
}

void
marmot_gobject_client_process_welcome_async(MarmotGobjectClient *self,
                                              const gchar *wrapper_event_id_hex,
                                              const gchar *rumor_event_json,
                                              GCancellable *cancellable,
                                              GAsyncReadyCallback callback,
                                              gpointer user_data)
{
    g_return_if_fail(MARMOT_GOBJECT_IS_CLIENT(self));
    g_return_if_fail(wrapper_event_id_hex != NULL);
    g_return_if_fail(rumor_event_json != NULL);
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    ProcessWelcomeData *d = g_new0(ProcessWelcomeData, 1);
    d->wrapper_event_id_hex = g_strdup(wrapper_event_id_hex);
    d->rumor_event_json     = g_strdup(rumor_event_json);
    g_task_set_task_data(task, d, process_welcome_data_free);
    g_task_run_in_thread(task, process_welcome_thread);
    g_object_unref(task);
}

MarmotGobjectWelcome *
marmot_gobject_client_process_welcome_finish(MarmotGobjectClient *self,
                                               GAsyncResult *result,
                                               GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, self), NULL);
    return g_task_propagate_pointer(G_TASK(result), error);
}

/* ── Accept welcome ────────────────────────────────────────────── */

static void
accept_welcome_thread(GTask *task, gpointer source_object,
                       gpointer task_data, GCancellable *cancellable)
{
    MarmotGobjectClient *self = MARMOT_GOBJECT_CLIENT(source_object);
    /* One libmarmot call at a time per client (a Marmot is not thread-safe). */
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->lock);
    MarmotGobjectWelcome *gobj = MARMOT_GOBJECT_WELCOME(task_data);
    (void)cancellable;

    const gchar *wrapper_hex = marmot_gobject_welcome_get_wrapper_event_id(gobj);
    if (!wrapper_hex) {
        g_task_return_new_error(task, MARMOT_GOBJECT_ERROR,
                                MARMOT_GOBJECT_ERROR_INVALID_INPUT,
                                "Welcome has no wrapper event ID");
        return;
    }

    uint8_t wrapper_id[32];
    if (!hex_to_bytes(wrapper_hex, wrapper_id, 32)) {
        g_task_return_new_error(task, MARMOT_GOBJECT_ERROR,
                                MARMOT_GOBJECT_ERROR_INVALID_HEX,
                                "Invalid wrapper event ID hex");
        return;
    }

    MarmotGroup *accepted_group = NULL;
    MarmotError err = marmot_accept_welcome_by_wrapper_id(self->marmot,
                                                           wrapper_id,
                                                           &accepted_group);
    if (err != MARMOT_OK) {
        g_task_return_new_error(task, MARMOT_GOBJECT_ERROR, (gint)err,
                                "%s", marmot_error_string(err));
        return;
    }

    if (accepted_group) {
        MarmotGobjectGroup *group = gobject_group_from_marmot(accepted_group);
        marmot_group_free(accepted_group);
        queue_client_signal(self, client_signals[SIGNAL_GROUP_JOINED], G_OBJECT(group));
        g_object_unref(group);
    }

    g_task_return_boolean(task, TRUE);
}

void
marmot_gobject_client_accept_welcome_async(MarmotGobjectClient *self,
                                             MarmotGobjectWelcome *welcome,
                                             GCancellable *cancellable,
                                             GAsyncReadyCallback callback,
                                             gpointer user_data)
{
    g_return_if_fail(MARMOT_GOBJECT_IS_CLIENT(self));
    g_return_if_fail(MARMOT_GOBJECT_IS_WELCOME(welcome));
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    g_task_set_task_data(task, g_object_ref(welcome), g_object_unref);
    g_task_run_in_thread(task, accept_welcome_thread);
    g_object_unref(task);
}

gboolean
marmot_gobject_client_accept_welcome_finish(MarmotGobjectClient *self,
                                              GAsyncResult *result,
                                              GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
    return g_task_propagate_boolean(G_TASK(result), error);
}

/* ══════════════════════════════════════════════════════════════════════════
 * MIP-03: Messages (async)
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    gchar *mls_group_id_hex;
    gchar *inner_event_json;
} SendMessageData;

static void
send_message_data_free(gpointer data)
{
    SendMessageData *d = data;
    g_free(d->mls_group_id_hex);
    g_free(d->inner_event_json);
    g_free(d);
}

static void
send_message_thread(GTask *task, gpointer source_object,
                     gpointer task_data, GCancellable *cancellable)
{
    MarmotGobjectClient *self = MARMOT_GOBJECT_CLIENT(source_object);
    /* One libmarmot call at a time per client (a Marmot is not thread-safe). */
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->lock);
    SendMessageData *d = task_data;
    (void)cancellable;

    /* Parse the MLS group ID from hex */
    uint8_t mls_group_id_bytes[128];
    size_t hex_len = strlen(d->mls_group_id_hex);
    size_t byte_len = hex_len / 2;
    if (byte_len > sizeof(mls_group_id_bytes) ||
        !hex_to_bytes(d->mls_group_id_hex, mls_group_id_bytes, byte_len)) {
        g_task_return_new_error(task, MARMOT_GOBJECT_ERROR,
                                MARMOT_GOBJECT_ERROR_INVALID_HEX,
                                "Invalid MLS group ID hex");
        return;
    }

    MarmotGroupId gid = marmot_group_id_new(mls_group_id_bytes, byte_len);

    MarmotOutgoingMessage result = { 0 };
    MarmotError err = marmot_create_message(
        self->marmot, &gid, d->inner_event_json, &result);
    marmot_group_id_free(&gid);

    if (err != MARMOT_OK) {
        g_task_return_new_error(task, MARMOT_GOBJECT_ERROR, (gint)err,
                                "%s", marmot_error_string(err));
        return;
    }

    gchar *event_json = g_strdup(result.event_json);
    marmot_outgoing_message_free(&result);
    g_task_return_pointer(task, event_json, g_free);
}

void
marmot_gobject_client_send_message_async(MarmotGobjectClient *self,
                                           const gchar *mls_group_id_hex,
                                           const gchar *inner_event_json,
                                           GCancellable *cancellable,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data)
{
    g_return_if_fail(MARMOT_GOBJECT_IS_CLIENT(self));
    g_return_if_fail(mls_group_id_hex != NULL);
    g_return_if_fail(inner_event_json != NULL);
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    SendMessageData *d = g_new0(SendMessageData, 1);
    d->mls_group_id_hex = g_strdup(mls_group_id_hex);
    d->inner_event_json = g_strdup(inner_event_json);
    g_task_set_task_data(task, d, send_message_data_free);
    g_task_run_in_thread(task, send_message_thread);
    g_object_unref(task);
}

gchar *
marmot_gobject_client_send_message_finish(MarmotGobjectClient *self,
                                            GAsyncResult *result,
                                            GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, self), NULL);
    return g_task_propagate_pointer(G_TASK(result), error);
}

/* ── Process Message ─────────────────────────────────────────────── */

typedef struct {
    gchar *group_event_json;
    MarmotGobjectMessageResultType result_type;
} ProcessMessageData;

static void
process_message_data_free(gpointer data)
{
    ProcessMessageData *d = data;
    g_free(d->group_event_json);
    g_free(d);
}

static void
process_message_thread(GTask *task, gpointer source_object,
                        gpointer task_data, GCancellable *cancellable)
{
    MarmotGobjectClient *self = MARMOT_GOBJECT_CLIENT(source_object);
    /* One libmarmot call at a time per client (a Marmot is not thread-safe). */
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->lock);
    ProcessMessageData *d = task_data;
    (void)cancellable;

    MarmotMessageResult result = { 0 };
    MarmotError err = marmot_process_message(
        self->marmot, d->group_event_json, &result);

    if (err != MARMOT_OK) {
        g_task_return_new_error(task, MARMOT_GOBJECT_ERROR, (gint)err,
                                "%s", marmot_error_string(err));
        return;
    }

    d->result_type = marmot_gobject_message_result_type_from_marmot(result.type);

    gchar *inner_json = NULL;
    if (result.type == MARMOT_RESULT_APPLICATION_MESSAGE && result.app_msg.inner_event_json) {
        inner_json = g_strdup(result.app_msg.inner_event_json);
        MarmotGobjectMessage *message = find_processed_message_for_inner_json(
            self->marmot, inner_json);
        if (message) {
            queue_client_signal(self, client_signals[SIGNAL_MESSAGE_RECEIVED], G_OBJECT(message));
            g_object_unref(message);
        }
    }

    if (result.type == MARMOT_RESULT_COMMIT && result.commit.updated_group) {
        MarmotGobjectGroup *group = gobject_group_from_marmot(result.commit.updated_group);
        if (group) {
            queue_client_signal(self, client_signals[SIGNAL_GROUP_UPDATED], G_OBJECT(group));
            g_object_unref(group);
        }
    }

    marmot_message_result_free(&result);
    g_task_return_pointer(task, inner_json, g_free);
}

void
marmot_gobject_client_process_message_async(MarmotGobjectClient *self,
                                              const gchar *group_event_json,
                                              GCancellable *cancellable,
                                              GAsyncReadyCallback callback,
                                              gpointer user_data)
{
    g_return_if_fail(MARMOT_GOBJECT_IS_CLIENT(self));
    g_return_if_fail(group_event_json != NULL);
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    ProcessMessageData *d = g_new0(ProcessMessageData, 1);
    d->group_event_json = g_strdup(group_event_json);
    g_task_set_task_data(task, d, process_message_data_free);
    g_task_run_in_thread(task, process_message_thread);
    g_object_unref(task);
}

gchar *
marmot_gobject_client_process_message_finish(MarmotGobjectClient *self,
                                               GAsyncResult *result,
                                               MarmotGobjectMessageResultType *out_result_type,
                                               GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, self), NULL);

    GTask *task = G_TASK(result);
    ProcessMessageData *d = g_task_get_task_data(task);

    /* Read result_type BEFORE propagate_pointer to avoid race condition */
    MarmotGobjectMessageResultType result_type = d->result_type;
    gchar *json = g_task_propagate_pointer(task, error);
    if (out_result_type)
        *out_result_type = result_type;
    return json;
}

/* ── Update group metadata ──────────────────────────────────────── */

typedef struct {
    gchar *mls_group_id_hex;
    gchar *name;
    gchar *description;
} UpdateMetadataData;

static void
update_metadata_data_free(gpointer data)
{
    UpdateMetadataData *d = data;
    g_free(d->mls_group_id_hex);
    g_free(d->name);
    g_free(d->description);
    g_free(d);
}

static void
update_metadata_thread(GTask *task, gpointer source_object,
                        gpointer task_data, GCancellable *cancellable)
{
    MarmotGobjectClient *self = MARMOT_GOBJECT_CLIENT(source_object);
    /* One libmarmot call at a time per client (a Marmot is not thread-safe). */
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->lock);
    UpdateMetadataData *d = task_data;
    (void)cancellable;

    uint8_t mls_group_id_bytes[128];
    size_t hex_len = strlen(d->mls_group_id_hex);
    size_t byte_len = hex_len / 2;
    if (hex_len % 2 != 0 || byte_len == 0 || byte_len > sizeof(mls_group_id_bytes) ||
        !hex_to_bytes(d->mls_group_id_hex, mls_group_id_bytes, byte_len)) {
        g_task_return_new_error(task, MARMOT_GOBJECT_ERROR,
                                MARMOT_GOBJECT_ERROR_INVALID_HEX,
                                "Invalid MLS group ID hex");
        return;
    }
    MarmotGroupId gid = marmot_group_id_new(mls_group_id_bytes, byte_len);

    MarmotGroupConfig config = { 0 };
    config.name = d->name;
    config.description = d->description;
    char *commit_json = NULL;
    MarmotError err = marmot_update_group_metadata(self->marmot, &gid, &config,
                                                   &commit_json);
    if (err != MARMOT_OK) {
        marmot_group_id_free(&gid);
        g_task_return_new_error(task, MARMOT_GOBJECT_ERROR, (gint)err,
                                "%s", marmot_error_string(err));
        return;
    }

    /* The Commit is pending: ::group-updated follows the merge. */
    marmot_group_id_free(&gid);

    gchar *json = g_strdup(commit_json);
    free(commit_json);
    g_task_return_pointer(task, json, g_free);
}

void
marmot_gobject_client_update_group_metadata_async(MarmotGobjectClient *self,
                                                   const gchar *mls_group_id_hex,
                                                   const gchar *name,
                                                   const gchar *description,
                                                   GCancellable *cancellable,
                                                   GAsyncReadyCallback callback,
                                                   gpointer user_data)
{
    g_return_if_fail(MARMOT_GOBJECT_IS_CLIENT(self));
    g_return_if_fail(mls_group_id_hex != NULL);
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    UpdateMetadataData *d = g_new0(UpdateMetadataData, 1);
    d->mls_group_id_hex = g_strdup(mls_group_id_hex);
    d->name = g_strdup(name);
    d->description = g_strdup(description);
    g_task_set_task_data(task, d, update_metadata_data_free);
    g_task_run_in_thread(task, update_metadata_thread);
    g_object_unref(task);
}

gchar *
marmot_gobject_client_update_group_metadata_finish(MarmotGobjectClient *self,
                                                    GAsyncResult *result,
                                                    GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, self), NULL);
    return g_task_propagate_pointer(G_TASK(result), error);
}

/* ── Pending Commit: merge / clear ──────────────────────────────── */

/* Task source tags (addresses, not function pointers: ISO C). */
static const char merge_pending_tag = 'm';
static const char clear_pending_tag = 'c';

static void
pending_commit_thread(GTask *task, gpointer source_object,
                      gpointer task_data, GCancellable *cancellable)
{
    MarmotGobjectClient *self = MARMOT_GOBJECT_CLIENT(source_object);
    /* One libmarmot call at a time per client (a Marmot is not thread-safe). */
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->lock);
    const gchar *gid_hex = task_data;
    gboolean merge = g_task_get_source_tag(task) == (gpointer)&merge_pending_tag;
    (void)cancellable;

    uint8_t bytes[128];
    size_t hex_len = strlen(gid_hex);
    size_t byte_len = hex_len / 2;
    if (hex_len % 2 != 0 || byte_len == 0 || byte_len > sizeof(bytes) ||
        !hex_to_bytes(gid_hex, bytes, byte_len)) {
        g_task_return_new_error(task, MARMOT_GOBJECT_ERROR,
                                MARMOT_GOBJECT_ERROR_INVALID_HEX,
                                "Invalid MLS group ID hex");
        return;
    }
    MarmotGroupId gid = marmot_group_id_new(bytes, byte_len);
    MarmotError err = merge ? marmot_merge_pending_commit(self->marmot, &gid)
                            : marmot_clear_pending_commit(self->marmot, &gid);
    /* Announce the group as stored now: merged, followed a Commit that beat
     * ours, or applied a deferred Commit after the clear. */
    MarmotGroup *updated = NULL;
    if ((err == MARMOT_OK || err == MARMOT_ERR_WRONG_EPOCH) &&
        marmot_get_group(self->marmot, &gid, &updated) == MARMOT_OK && updated) {
        MarmotGobjectGroup *group = gobject_group_from_marmot(updated);
        if (group) {
            queue_client_signal(self, client_signals[SIGNAL_GROUP_UPDATED], G_OBJECT(group));
            g_object_unref(group);
        }
        marmot_group_free(updated);
    }
    marmot_group_id_free(&gid);
    if (err != MARMOT_OK) {
        g_task_return_new_error(task, MARMOT_GOBJECT_ERROR, (gint)err,
                                "%s", marmot_error_string(err));
        return;
    }
    g_task_return_boolean(task, TRUE);
}

static void
run_pending_commit_task(MarmotGobjectClient *self, gpointer tag,
                        const gchar *mls_group_id_hex, GCancellable *cancellable,
                        GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new(self, cancellable, callback, user_data);
    g_task_set_source_tag(task, tag);
    g_task_set_task_data(task, g_strdup(mls_group_id_hex), g_free);
    g_task_run_in_thread(task, pending_commit_thread);
    g_object_unref(task);
}

void
marmot_gobject_client_merge_pending_commit_async(MarmotGobjectClient *self,
                                                  const gchar *mls_group_id_hex,
                                                  GCancellable *cancellable,
                                                  GAsyncReadyCallback callback,
                                                  gpointer user_data)
{
    g_return_if_fail(MARMOT_GOBJECT_IS_CLIENT(self));
    g_return_if_fail(mls_group_id_hex != NULL);
    run_pending_commit_task(self, (gpointer)&merge_pending_tag,
                            mls_group_id_hex, cancellable, callback, user_data);
}

gboolean
marmot_gobject_client_merge_pending_commit_finish(MarmotGobjectClient *self,
                                                   GAsyncResult *result,
                                                   GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
    return g_task_propagate_boolean(G_TASK(result), error);
}

void
marmot_gobject_client_clear_pending_commit_async(MarmotGobjectClient *self,
                                                  const gchar *mls_group_id_hex,
                                                  GCancellable *cancellable,
                                                  GAsyncReadyCallback callback,
                                                  gpointer user_data)
{
    g_return_if_fail(MARMOT_GOBJECT_IS_CLIENT(self));
    g_return_if_fail(mls_group_id_hex != NULL);
    run_pending_commit_task(self, (gpointer)&clear_pending_tag,
                            mls_group_id_hex, cancellable, callback, user_data);
}

gboolean
marmot_gobject_client_clear_pending_commit_finish(MarmotGobjectClient *self,
                                                   GAsyncResult *result,
                                                   GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
    return g_task_propagate_boolean(G_TASK(result), error);
}

/* ══════════════════════════════════════════════════════════════════════════
 * MIP-04: Media Encryption (async)
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    gchar *mls_group_id_hex;
    guint8 *file_data;
    gsize file_len;
    gchar *mime_type;
    gchar *filename;
} EncryptMediaData;

static void
encrypt_media_data_free(gpointer data)
{
    EncryptMediaData *d = data;
    g_free(d->mls_group_id_hex);
    g_free(d->file_data);
    g_free(d->mime_type);
    g_free(d->filename);
    g_free(d);
}

static void
encrypt_media_thread(GTask *task, gpointer source_object,
                      gpointer task_data, GCancellable *cancellable)
{
    MarmotGobjectClient *self = MARMOT_GOBJECT_CLIENT(source_object);
    /* One libmarmot call at a time per client (a Marmot is not thread-safe). */
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->lock);
    EncryptMediaData *d = task_data;
    (void)cancellable;

    size_t hex_len = strlen(d->mls_group_id_hex);
    size_t byte_len = hex_len / 2;
    uint8_t mls_gid_bytes[128];
    if (byte_len > sizeof(mls_gid_bytes) ||
        !hex_to_bytes(d->mls_group_id_hex, mls_gid_bytes, byte_len)) {
        g_task_return_new_error(task, MARMOT_GOBJECT_ERROR,
                                MARMOT_GOBJECT_ERROR_INVALID_HEX,
                                "Invalid MLS group ID hex");
        return;
    }

    MarmotGroupId gid = marmot_group_id_new(mls_gid_bytes, byte_len);

    MarmotEncryptedMedia result = { 0 };
    MarmotError err = marmot_encrypt_media(
        self->marmot, &gid,
        d->file_data, d->file_len,
        d->mime_type, d->filename,
        &result);
    marmot_group_id_free(&gid);

    if (err != MARMOT_OK) {
        g_task_return_new_error(task, MARMOT_GOBJECT_ERROR, (gint)err,
                                "%s", marmot_error_string(err));
        return;
    }

    GBytes *encrypted = g_bytes_new_take(result.encrypted_data, result.encrypted_len);
    result.encrypted_data = NULL;  /* ownership transferred */
    MarmotGobjectEncryptedMedia *media = marmot_gobject_encrypted_media_new(
        encrypted,
        result.imeta.mime_type,
        result.imeta.filename,
        result.imeta.url,
        result.imeta.original_size,
        result.imeta.file_hash,
        result.imeta.nonce,
        result.imeta.epoch);
    g_bytes_unref(encrypted);
    marmot_encrypted_media_clear(&result);
    g_task_return_pointer(task, media, (GDestroyNotify)marmot_gobject_encrypted_media_free);
}

void
marmot_gobject_client_encrypt_media_async(MarmotGobjectClient *self,
                                            const gchar *mls_group_id_hex,
                                            GBytes *file_data,
                                            const gchar *mime_type,
                                            const gchar *filename,
                                            GCancellable *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data)
{
    g_return_if_fail(MARMOT_GOBJECT_IS_CLIENT(self));
    g_return_if_fail(mls_group_id_hex != NULL);
    g_return_if_fail(file_data != NULL);

    GTask *task = g_task_new(self, cancellable, callback, user_data);

    gsize data_len;
    const guint8 *data = g_bytes_get_data(file_data, &data_len);

    EncryptMediaData *d = g_new0(EncryptMediaData, 1);
    d->mls_group_id_hex = g_strdup(mls_group_id_hex);
    d->file_data = g_memdup2(data, data_len);
    d->file_len = data_len;
    d->mime_type = g_strdup(mime_type);
    d->filename = g_strdup(filename);
    g_task_set_task_data(task, d, encrypt_media_data_free);

    g_task_run_in_thread(task, encrypt_media_thread);
    g_object_unref(task);
}

MarmotGobjectEncryptedMedia *
marmot_gobject_client_encrypt_media_finish(MarmotGobjectClient *self,
                                            GAsyncResult *result,
                                            GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, self), NULL);
    return g_task_propagate_pointer(G_TASK(result), error);
}

/* ── Decrypt Media ────────────────────────────────────────────── */

typedef struct {
    gchar *mls_group_id_hex;
    guint8 *encrypted_data;
    gsize encrypted_len;
    MarmotImetaInfo imeta;
} DecryptMediaData;

static void
decrypt_media_data_free(gpointer data)
{
    DecryptMediaData *d = data;
    g_free(d->mls_group_id_hex);
    g_free(d->encrypted_data);
    g_free(d->imeta.mime_type);
    g_free(d->imeta.filename);
    g_free(d->imeta.url);
    g_free(d);
}

static void
decrypt_media_thread(GTask *task, gpointer source_object,
                      gpointer task_data, GCancellable *cancellable)
{
    MarmotGobjectClient *self = MARMOT_GOBJECT_CLIENT(source_object);
    /* One libmarmot call at a time per client (a Marmot is not thread-safe). */
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->lock);
    DecryptMediaData *d = task_data;
    (void)cancellable;

    size_t hex_len = strlen(d->mls_group_id_hex);
    size_t byte_len = hex_len / 2;
    uint8_t mls_gid_bytes[128];
    if (byte_len > sizeof(mls_gid_bytes) ||
        !hex_to_bytes(d->mls_group_id_hex, mls_gid_bytes, byte_len)) {
        g_task_return_new_error(task, MARMOT_GOBJECT_ERROR,
                                MARMOT_GOBJECT_ERROR_INVALID_HEX,
                                "Invalid MLS group ID hex");
        return;
    }

    MarmotGroupId gid = marmot_group_id_new(mls_gid_bytes, byte_len);

    uint8_t *plaintext = NULL;
    size_t plaintext_len = 0;
    MarmotError err = marmot_decrypt_media(
        self->marmot, &gid,
        d->encrypted_data, d->encrypted_len,
        &d->imeta,
        &plaintext, &plaintext_len);
    marmot_group_id_free(&gid);

    if (err != MARMOT_OK) {
        g_task_return_new_error(task, MARMOT_GOBJECT_ERROR, (gint)err,
                                "%s", marmot_error_string(err));
        return;
    }

    GBytes *result = g_bytes_new_take(plaintext, plaintext_len);
    g_task_return_pointer(task, result, (GDestroyNotify)g_bytes_unref);
}

void
marmot_gobject_client_decrypt_media_async(MarmotGobjectClient *self,
                                            const gchar *mls_group_id_hex,
                                            GBytes *encrypted_data,
                                            const gchar *mime_type,
                                            const gchar *filename,
                                            gsize original_size,
                                            const guint8 file_hash[32],
                                            const guint8 nonce[12],
                                            guint64 epoch,
                                            GCancellable *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data)
{
    g_return_if_fail(MARMOT_GOBJECT_IS_CLIENT(self));
    g_return_if_fail(mls_group_id_hex != NULL);
    g_return_if_fail(encrypted_data != NULL);

    GTask *task = g_task_new(self, cancellable, callback, user_data);

    gsize enc_len;
    const guint8 *enc_bytes = g_bytes_get_data(encrypted_data, &enc_len);

    DecryptMediaData *d = g_new0(DecryptMediaData, 1);
    d->mls_group_id_hex = g_strdup(mls_group_id_hex);
    d->encrypted_data = g_memdup2(enc_bytes, enc_len);
    d->encrypted_len = enc_len;
    d->imeta.mime_type = g_strdup(mime_type);
    d->imeta.filename = g_strdup(filename);
    d->imeta.original_size = original_size;
    d->imeta.epoch = epoch;
    if (file_hash)
        memcpy(d->imeta.file_hash, file_hash, 32);
    if (nonce)
        memcpy(d->imeta.nonce, nonce, 12);
    g_task_set_task_data(task, d, decrypt_media_data_free);

    g_task_run_in_thread(task, decrypt_media_thread);
    g_object_unref(task);
}

GBytes *
marmot_gobject_client_decrypt_media_finish(MarmotGobjectClient *self,
                                            GAsyncResult *result,
                                            GError **error)
{
    g_return_val_if_fail(g_task_is_valid(result, self), NULL);
    return g_task_propagate_pointer(G_TASK(result), error);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Synchronous queries
 * ══════════════════════════════════════════════════════════════════════════ */

MarmotGobjectGroup *
marmot_gobject_client_get_group(MarmotGobjectClient *self,
                                 const gchar *mls_group_id_hex,
                                 GError **error)
{
    g_return_val_if_fail(MARMOT_GOBJECT_IS_CLIENT(self), NULL);
    g_return_val_if_fail(mls_group_id_hex != NULL, NULL);
    /* One libmarmot call at a time per client (a Marmot is not thread-safe). */
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->lock);

    size_t hex_len = strlen(mls_group_id_hex);
    size_t byte_len = hex_len / 2;
    uint8_t *bytes = g_malloc(byte_len);
    if (!hex_to_bytes(mls_group_id_hex, bytes, byte_len)) {
        g_free(bytes);
        g_set_error(error, MARMOT_GOBJECT_ERROR,
                    MARMOT_GOBJECT_ERROR_INVALID_HEX, "Invalid hex");
        return NULL;
    }

    MarmotGroupId gid = marmot_group_id_new(bytes, byte_len);
    g_free(bytes);

    MarmotGroup *group = NULL;
    MarmotError err = marmot_get_group(self->marmot, &gid, &group);
    marmot_group_id_free(&gid);

    if (err != MARMOT_OK) {
        set_marmot_error(error, err);
        return NULL;
    }
    if (!group)
        return NULL;

    MarmotGobjectGroup *gobj = gobject_group_from_marmot(group);
    marmot_group_free(group);
    return gobj;
}

GPtrArray *
marmot_gobject_client_get_all_groups(MarmotGobjectClient *self, GError **error)
{
    g_return_val_if_fail(MARMOT_GOBJECT_IS_CLIENT(self), NULL);
    /* One libmarmot call at a time per client (a Marmot is not thread-safe). */
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->lock);

    MarmotGroup **groups = NULL;
    size_t count = 0;
    MarmotError err = marmot_get_all_groups(self->marmot, &groups, &count);
    if (err != MARMOT_OK) {
        set_marmot_error(error, err);
        return NULL;
    }

    GPtrArray *arr = g_ptr_array_new_with_free_func(g_object_unref);
    for (size_t i = 0; i < count; i++) {
        MarmotGobjectGroup *g = gobject_group_from_marmot(groups[i]);
        g_ptr_array_add(arr, g);
        marmot_group_free(groups[i]);
    }
    free(groups);
    return arr;
}

GPtrArray *
marmot_gobject_client_get_messages(MarmotGobjectClient *self,
                                     const gchar *mls_group_id_hex,
                                     guint limit, guint offset,
                                     GError **error)
{
    g_return_val_if_fail(MARMOT_GOBJECT_IS_CLIENT(self), NULL);
    /* One libmarmot call at a time per client (a Marmot is not thread-safe). */
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->lock);

    size_t hex_len = strlen(mls_group_id_hex);
    size_t byte_len = hex_len / 2;
    uint8_t *bytes = g_malloc(byte_len);
    if (!hex_to_bytes(mls_group_id_hex, bytes, byte_len)) {
        g_free(bytes);
        g_set_error(error, MARMOT_GOBJECT_ERROR,
                    MARMOT_GOBJECT_ERROR_INVALID_HEX, "Invalid hex");
        return NULL;
    }

    MarmotGroupId gid = marmot_group_id_new(bytes, byte_len);
    g_free(bytes);

    MarmotPagination page = marmot_pagination_default();
    if (limit > 0) page.limit = limit;
    page.offset = offset;

    MarmotMessage **msgs = NULL;
    size_t count = 0;
    MarmotError err = marmot_get_messages(self->marmot, &gid, &page, &msgs, &count);
    marmot_group_id_free(&gid);

    if (err != MARMOT_OK) {
        set_marmot_error(error, err);
        return NULL;
    }

    GPtrArray *arr = g_ptr_array_new_with_free_func(g_object_unref);
    for (size_t i = 0; i < count; i++) {
        MarmotGobjectMessage *m = gobject_message_from_marmot(msgs[i]);
        g_ptr_array_add(arr, m);
        marmot_message_free(msgs[i]);
    }
    free(msgs);
    return arr;
}

GPtrArray *
marmot_gobject_client_get_pending_welcomes(MarmotGobjectClient *self, GError **error)
{
    g_return_val_if_fail(MARMOT_GOBJECT_IS_CLIENT(self), NULL);
    /* One libmarmot call at a time per client (a Marmot is not thread-safe). */
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->lock);

    MarmotWelcome **welcomes = NULL;
    size_t count = 0;
    MarmotError err = marmot_get_pending_welcomes(self->marmot, NULL, &welcomes, &count);
    if (err != MARMOT_OK) {
        set_marmot_error(error, err);
        return NULL;
    }

    GPtrArray *arr = g_ptr_array_new_with_free_func(g_object_unref);
    for (size_t i = 0; i < count; i++) {
        MarmotGobjectWelcome *w = gobject_welcome_from_marmot(welcomes[i]);
        g_ptr_array_add(arr, w);
        marmot_welcome_free(welcomes[i]);
    }
    free(welcomes);
    return arr;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Internal access
 * ══════════════════════════════════════════════════════════════════════════ */

void
marmot_gobject_client_lock(MarmotGobjectClient *self)
{
    g_return_if_fail(MARMOT_GOBJECT_IS_CLIENT(self));
    g_mutex_lock(&self->lock);
}

void
marmot_gobject_client_unlock(MarmotGobjectClient *self)
{
    g_return_if_fail(MARMOT_GOBJECT_IS_CLIENT(self));
    g_mutex_unlock(&self->lock);
}

Marmot *
marmot_gobject_client_get_marmot(MarmotGobjectClient *self)
{
    g_return_val_if_fail(MARMOT_GOBJECT_IS_CLIENT(self), NULL);
    return self->marmot;
}

gchar **
marmot_gobject_client_get_group_relay_urls(MarmotGobjectClient *self,
                                            const gchar *mls_group_id_hex,
                                            gsize *out_count)
{
    g_return_val_if_fail(MARMOT_GOBJECT_IS_CLIENT(self), NULL);
    g_return_val_if_fail(mls_group_id_hex != NULL, NULL);
    /* One libmarmot call at a time per client (a Marmot is not thread-safe). */
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->lock);

    if (out_count) *out_count = 0;

    gsize hex_len = strlen(mls_group_id_hex);
    gsize gid_len = hex_len / 2;
    uint8_t *gid_bytes = g_malloc(gid_len);
    if (!hex_to_bytes(mls_group_id_hex, gid_bytes, gid_len)) {
        g_free(gid_bytes);
        return NULL;
    }

    MarmotGroupId mls_gid = marmot_group_id_new(gid_bytes, gid_len);
    g_free(gid_bytes);

    MarmotGroupRelay *relays = NULL;
    size_t relay_count = 0;
    MarmotError err = marmot_get_group_relay_urls(self->marmot, &mls_gid,
                                                   &relays, &relay_count);
    marmot_group_id_free(&mls_gid);

    if (err != MARMOT_OK || relay_count == 0) {
        if (relays) {
            for (size_t i = 0; i < relay_count; i++) {
                free(relays[i].relay_url);
                marmot_group_id_free(&relays[i].mls_group_id);
            }
            free(relays);
        }
        return NULL;
    }

    /* Convert to GLib strv */
    gchar **urls = g_new0(gchar *, relay_count + 1);
    for (size_t i = 0; i < relay_count; i++) {
        urls[i] = g_strdup(relays[i].relay_url);
        free(relays[i].relay_url);
        marmot_group_id_free(&relays[i].mls_group_id);
    }
    urls[relay_count] = NULL;
    free(relays);

    if (out_count) *out_count = relay_count;
    return urls;
}

/* ── Pending Commit / unsent Welcomes (restart path, review R2) ──── */

static gboolean
parse_group_id_hex(const gchar *hex, MarmotGroupId *out, GError **error)
{
    uint8_t bytes[128];
    size_t hex_len = strlen(hex);
    size_t byte_len = hex_len / 2;
    if (hex_len % 2 != 0 || byte_len == 0 || byte_len > sizeof(bytes) ||
        !hex_to_bytes(hex, bytes, byte_len)) {
        g_set_error(error, MARMOT_GOBJECT_ERROR, MARMOT_GOBJECT_ERROR_INVALID_HEX,
                    "Invalid MLS group ID hex");
        return FALSE;
    }
    *out = marmot_group_id_new(bytes, byte_len);
    return TRUE;
}

gchar *
marmot_gobject_client_get_pending_commit(MarmotGobjectClient *self,
                                         const gchar *mls_group_id_hex,
                                         gboolean *out_superseded,
                                         GError **error)
{
    g_return_val_if_fail(MARMOT_GOBJECT_IS_CLIENT(self), NULL);
    g_return_val_if_fail(mls_group_id_hex != NULL, NULL);
    /* One libmarmot call at a time per client (a Marmot is not thread-safe). */
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->lock);
    if (out_superseded) *out_superseded = FALSE;
    MarmotGroupId gid;
    if (!parse_group_id_hex(mls_group_id_hex, &gid, error)) return NULL;
    char *ev = NULL;
    bool superseded = false;
    MarmotError err = marmot_get_pending_commit(self->marmot, &gid, &ev, &superseded);
    marmot_group_id_free(&gid);
    if (err != MARMOT_OK) {
        g_set_error(error, MARMOT_GOBJECT_ERROR, (gint)err, "%s", marmot_error_string(err));
        return NULL;
    }
    if (out_superseded) *out_superseded = superseded;
    gchar *json = g_strdup(ev);
    free(ev);
    return json;
}

gboolean
marmot_gobject_client_get_unsent_welcomes(MarmotGobjectClient *self,
                                          const gchar *mls_group_id_hex,
                                          gchar ***out_ids_hex,
                                          gchar ***out_rumors,
                                          gchar ***out_recipients_hex,
                                          GError **error)
{
    g_return_val_if_fail(MARMOT_GOBJECT_IS_CLIENT(self), FALSE);
    g_return_val_if_fail(mls_group_id_hex && out_ids_hex && out_rumors &&
                         out_recipients_hex, FALSE);
    /* One libmarmot call at a time per client (a Marmot is not thread-safe). */
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->lock);
    *out_ids_hex = NULL;
    *out_rumors = NULL;
    *out_recipients_hex = NULL;
    MarmotGroupId gid;
    if (!parse_group_id_hex(mls_group_id_hex, &gid, error)) return FALSE;
    MarmotUnsentWelcome *w = NULL;
    size_t n = 0;
    MarmotError err = marmot_get_unsent_welcomes(self->marmot, &gid, &w, &n);
    marmot_group_id_free(&gid);
    if (err != MARMOT_OK) {
        g_set_error(error, MARMOT_GOBJECT_ERROR, (gint)err, "%s", marmot_error_string(err));
        return FALSE;
    }
    gchar **ids = g_new0(gchar *, n + 1);
    gchar **rumors = g_new0(gchar *, n + 1);
    gchar **recipients = g_new0(gchar *, n + 1);
    for (size_t i = 0; i < n; i++) {
        ids[i] = bytes_to_hex(w[i].id, 32);
        rumors[i] = g_strdup(w[i].rumor_json);
        recipients[i] = bytes_to_hex(w[i].recipient, 32);
    }
    marmot_unsent_welcomes_free(w, n);
    *out_ids_hex = ids;
    *out_rumors = rumors;
    *out_recipients_hex = recipients;
    return TRUE;
}

gboolean
marmot_gobject_client_mark_welcomes_sent(MarmotGobjectClient *self,
                                         const gchar *mls_group_id_hex,
                                         const gchar * const *ids_hex,
                                         GError **error)
{
    g_return_val_if_fail(MARMOT_GOBJECT_IS_CLIENT(self), FALSE);
    g_return_val_if_fail(mls_group_id_hex != NULL, FALSE);
    /* One libmarmot call at a time per client (a Marmot is not thread-safe). */
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->lock);
    MarmotGroupId gid;
    if (!parse_group_id_hex(mls_group_id_hex, &gid, error)) return FALSE;
    guint n = ids_hex ? g_strv_length((gchar **) ids_hex) : 0;
    uint8_t (*ids)[32] = n ? g_malloc(n * 32) : NULL;
    for (guint i = 0; i < n; i++) {
        if (strlen(ids_hex[i]) != 64 || !hex_to_bytes(ids_hex[i], ids[i], 32)) {
            g_free(ids);
            marmot_group_id_free(&gid);
            g_set_error(error, MARMOT_GOBJECT_ERROR, MARMOT_GOBJECT_ERROR_INVALID_HEX,
                        "Invalid Welcome id hex");
            return FALSE;
        }
    }
    MarmotError err = marmot_mark_welcomes_sent(self->marmot, &gid,
                                                (const uint8_t (*)[32]) ids, n);
    g_free(ids);
    marmot_group_id_free(&gid);
    if (err != MARMOT_OK) {
        g_set_error(error, MARMOT_GOBJECT_ERROR, (gint)err, "%s", marmot_error_string(err));
        return FALSE;
    }
    return TRUE;
}
