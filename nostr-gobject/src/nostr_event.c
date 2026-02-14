/**
 * SPDX-License-Identifier: MIT
 *
 * GNostrEvent: GObject wrapper for Nostr events (NIP-01)
 *
 * Provides a modern GObject implementation with:
 * - Properties with notify signals
 * - "signed" and "verified" signals
 * - GError-based error handling
 * - JSON serialization/deserialization
 */

#include "nostr_event.h"
#include <glib.h>

/* Core libnostr headers */
#include "nostr-event.h"

/* Property IDs */
enum {
    PROP_0,
    PROP_ID,
    PROP_PUBKEY,
    PROP_CREATED_AT,
    PROP_KIND,
    PROP_CONTENT,
    PROP_SIG,
    PROP_TAGS,
    N_PROPERTIES
};

/* Signal indices (internal) */
enum {
    SIGNAL_SIGNED,
    SIGNAL_VERIFIED,
    N_SIGNALS
};

static GParamSpec *obj_properties[N_PROPERTIES] = { NULL, };
static guint gnostr_event_signals[N_SIGNALS] = { 0 };

struct _GNostrEvent {
    GObject parent_instance;
    NostrEvent *event;  /* Core libnostr event */
};

G_DEFINE_TYPE(GNostrEvent, gnostr_event, G_TYPE_OBJECT)

static void
gnostr_event_set_property(GObject      *object,
                          guint         property_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
    GNostrEvent *self = GNOSTR_EVENT(object);

    switch (property_id) {
    case PROP_CREATED_AT:
        gnostr_event_set_created_at(self, g_value_get_int64(value));
        break;
    case PROP_KIND:
        gnostr_event_set_kind(self, g_value_get_uint(value));
        break;
    case PROP_CONTENT:
        gnostr_event_set_content(self, g_value_get_string(value));
        break;
    case PROP_TAGS:
        gnostr_event_set_tags(self, g_value_get_pointer(value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
gnostr_event_get_property(GObject    *object,
                          guint       property_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
    GNostrEvent *self = GNOSTR_EVENT(object);

    switch (property_id) {
    case PROP_ID:
        g_value_set_string(value, gnostr_event_get_id(self));
        break;
    case PROP_PUBKEY:
        g_value_set_string(value, gnostr_event_get_pubkey(self));
        break;
    case PROP_CREATED_AT:
        g_value_set_int64(value, gnostr_event_get_created_at(self));
        break;
    case PROP_KIND:
        g_value_set_uint(value, gnostr_event_get_kind(self));
        break;
    case PROP_CONTENT:
        g_value_set_string(value, gnostr_event_get_content(self));
        break;
    case PROP_SIG:
        g_value_set_string(value, gnostr_event_get_sig(self));
        break;
    case PROP_TAGS:
        g_value_set_pointer(value, gnostr_event_get_tags(self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
gnostr_event_finalize(GObject *object)
{
    GNostrEvent *self = GNOSTR_EVENT(object);

    if (self->event) {
        nostr_event_free(self->event);
        self->event = NULL;
    }

    G_OBJECT_CLASS(gnostr_event_parent_class)->finalize(object);
}

static void
gnostr_event_class_init(GNostrEventClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->set_property = gnostr_event_set_property;
    object_class->get_property = gnostr_event_get_property;
    object_class->finalize = gnostr_event_finalize;

    /**
     * GNostrEvent:id:
     *
     * The event ID (32-byte hex string). Read-only after signing.
     */
    obj_properties[PROP_ID] =
        g_param_spec_string("id",
                            "ID",
                            "Event ID (read-only after signing)",
                            NULL,
                            G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

    /**
     * GNostrEvent:pubkey:
     *
     * The author's public key (32-byte hex string). Read-only.
     */
    obj_properties[PROP_PUBKEY] =
        g_param_spec_string("pubkey",
                            "Public Key",
                            "Author's public key (read-only)",
                            NULL,
                            G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

    /**
     * GNostrEvent:created-at:
     *
     * Unix timestamp of event creation.
     */
    obj_properties[PROP_CREATED_AT] =
        g_param_spec_int64("created-at",
                           "Created At",
                           "Unix timestamp of creation",
                           G_MININT64, G_MAXINT64, 0,
                           G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

    /**
     * GNostrEvent:kind:
     *
     * The event kind (NIP-01 defined types).
     */
    obj_properties[PROP_KIND] =
        g_param_spec_uint("kind",
                          "Kind",
                          "Event kind",
                          0, G_MAXUINT, 0,
                          G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

    /**
     * GNostrEvent:content:
     *
     * The event content string.
     */
    obj_properties[PROP_CONTENT] =
        g_param_spec_string("content",
                            "Content",
                            "Event content",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

    /**
     * GNostrEvent:sig:
     *
     * The Schnorr signature (64-byte hex string). Read-only after signing.
     */
    obj_properties[PROP_SIG] =
        g_param_spec_string("sig",
                            "Signature",
                            "Event signature (read-only after signing)",
                            NULL,
                            G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

    /**
     * GNostrEvent:tags:
     *
     * Event tags (pointer to NostrTags). Use libnostr tag functions.
     */
    obj_properties[PROP_TAGS] =
        g_param_spec_pointer("tags",
                             "Tags",
                             "Event tags (NostrTags pointer)",
                             G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPERTIES, obj_properties);

    /**
     * GNostrEvent::signed:
     * @self: the event that was signed
     *
     * Emitted after the event has been successfully signed.
     */
    gnostr_event_signals[SIGNAL_SIGNED] =
        g_signal_new("signed",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 0);

    /**
     * GNostrEvent::verified:
     * @self: the event that was verified
     *
     * Emitted after the event signature has been successfully verified.
     */
    gnostr_event_signals[SIGNAL_VERIFIED] =
        g_signal_new("verified",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_FIRST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 0);
}

static void
gnostr_event_init(GNostrEvent *self)
{
    self->event = nostr_event_new();
}

/* Public API */

GNostrEvent *
gnostr_event_new(void)
{
    return g_object_new(GNOSTR_TYPE_EVENT, NULL);
}

GNostrEvent *
gnostr_event_new_from_json(const gchar *json, GError **error)
{
    g_return_val_if_fail(json != NULL, NULL);

    GNostrEvent *self = gnostr_event_new();

    if (!self->event) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_INVALID_EVENT,
                    "Failed to allocate event");
        g_object_unref(self);
        return NULL;
    }

    int result = nostr_event_deserialize_compact(self->event, json, NULL);
    if (result == 0) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_PARSE_FAILED,
                    "Failed to parse JSON event");
        g_object_unref(self);
        return NULL;
    }

    return self;
}

gchar *
gnostr_event_to_json(GNostrEvent *self)
{
    g_return_val_if_fail(GNOSTR_IS_EVENT(self), NULL);
    g_return_val_if_fail(self->event != NULL, NULL);

    return nostr_event_serialize_compact(self->event);
}

gboolean
gnostr_event_sign(GNostrEvent *self, const gchar *privkey, GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_EVENT(self), FALSE);
    g_return_val_if_fail(privkey != NULL, FALSE);
    g_return_val_if_fail(self->event != NULL, FALSE);

    /* Validate private key format (64 hex chars) */
    gsize len = strlen(privkey);
    if (len != 64) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_INVALID_KEY,
                    "Invalid private key: expected 64 hex characters, got %zu", len);
        return FALSE;
    }

    int result = nostr_event_sign(self->event, privkey);
    if (result != 0) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_SIGNATURE_FAILED,
                    "Failed to sign event (error code: %d)", result);
        return FALSE;
    }

    /* Notify property changes */
    g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_ID]);
    g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_PUBKEY]);
    g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_SIG]);

    /* Emit signed signal */
    g_signal_emit(self, gnostr_event_signals[SIGNAL_SIGNED], 0);

    return TRUE;
}

gboolean
gnostr_event_verify(GNostrEvent *self, GError **error)
{
    g_return_val_if_fail(GNOSTR_IS_EVENT(self), FALSE);
    g_return_val_if_fail(self->event != NULL, FALSE);

    gboolean valid = nostr_event_check_signature(self->event);
    if (!valid) {
        g_set_error(error,
                    NOSTR_ERROR,
                    NOSTR_ERROR_SIGNATURE_INVALID,
                    "Event signature verification failed");
        return FALSE;
    }

    /* Emit verified signal */
    g_signal_emit(self, gnostr_event_signals[SIGNAL_VERIFIED], 0);

    return TRUE;
}

/* Property accessors */

const gchar *
gnostr_event_get_id(GNostrEvent *self)
{
    g_return_val_if_fail(GNOSTR_IS_EVENT(self), NULL);

    if (self->event == NULL)
        return NULL;

    return self->event->id;
}

const gchar *
gnostr_event_get_pubkey(GNostrEvent *self)
{
    g_return_val_if_fail(GNOSTR_IS_EVENT(self), NULL);

    if (self->event == NULL)
        return NULL;

    return nostr_event_get_pubkey(self->event);
}

gint64
gnostr_event_get_created_at(GNostrEvent *self)
{
    g_return_val_if_fail(GNOSTR_IS_EVENT(self), 0);

    if (self->event == NULL)
        return 0;

    return nostr_event_get_created_at(self->event);
}

void
gnostr_event_set_created_at(GNostrEvent *self, gint64 created_at)
{
    g_return_if_fail(GNOSTR_IS_EVENT(self));
    g_return_if_fail(self->event != NULL);

    gint64 old_value = nostr_event_get_created_at(self->event);
    if (old_value == created_at)
        return;

    nostr_event_set_created_at(self->event, created_at);
    g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_CREATED_AT]);
}

guint
gnostr_event_get_kind(GNostrEvent *self)
{
    g_return_val_if_fail(GNOSTR_IS_EVENT(self), 0);

    if (self->event == NULL)
        return 0;

    return (guint)nostr_event_get_kind(self->event);
}

void
gnostr_event_set_kind(GNostrEvent *self, guint kind)
{
    g_return_if_fail(GNOSTR_IS_EVENT(self));
    g_return_if_fail(self->event != NULL);

    guint old_value = (guint)nostr_event_get_kind(self->event);
    if (old_value == kind)
        return;

    nostr_event_set_kind(self->event, (int)kind);
    g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_KIND]);
}

const gchar *
gnostr_event_get_content(GNostrEvent *self)
{
    g_return_val_if_fail(GNOSTR_IS_EVENT(self), NULL);

    if (self->event == NULL)
        return NULL;

    return nostr_event_get_content(self->event);
}

void
gnostr_event_set_content(GNostrEvent *self, const gchar *content)
{
    g_return_if_fail(GNOSTR_IS_EVENT(self));
    g_return_if_fail(self->event != NULL);

    const gchar *old_value = nostr_event_get_content(self->event);
    if (g_strcmp0(old_value, content) == 0)
        return;

    nostr_event_set_content(self->event, content);
    g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_CONTENT]);
}

const gchar *
gnostr_event_get_sig(GNostrEvent *self)
{
    g_return_val_if_fail(GNOSTR_IS_EVENT(self), NULL);

    if (self->event == NULL)
        return NULL;

    return nostr_event_get_sig(self->event);
}

gpointer
gnostr_event_get_tags(GNostrEvent *self)
{
    g_return_val_if_fail(GNOSTR_IS_EVENT(self), NULL);

    if (self->event == NULL)
        return NULL;

    return nostr_event_get_tags(self->event);
}

void
gnostr_event_set_tags(GNostrEvent *self, gpointer tags)
{
    g_return_if_fail(GNOSTR_IS_EVENT(self));
    g_return_if_fail(self->event != NULL);

    gpointer old_tags = nostr_event_get_tags(self->event);
    if (old_tags == tags)
        return;

    nostr_event_set_tags(self->event, tags);
    g_object_notify_by_pspec(G_OBJECT(self), obj_properties[PROP_TAGS]);
}
