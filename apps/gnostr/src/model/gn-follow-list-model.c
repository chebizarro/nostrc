/**
 * GnFollowListModel - A GListModel for NIP-02 follow lists
 *
 * Provides follows from a user's kind:3 contact list as a GListModel
 * for use with GtkListView. Supports incremental loading and profile
 * metadata resolution.
 */

#include "gn-follow-list-model.h"
#include <nostr-gobject-1.0/storage_ndb.h>
#include "../util/follow_list.h"
#include <nostr-gobject-1.0/nostr_profile_service.h>
#include <nostr-gobject-1.0/nostr_json.h>
#include <json.h>
#include <string.h>

/* ========== GnFollowListItem ========== */

struct _GnFollowListItem {
    GObject parent_instance;

    gchar *pubkey;
    gchar *relay_hint;
    gchar *petname;

    /* Resolved profile metadata */
    gchar *display_name;
    gchar *nip05;
    gchar *picture_url;
    gboolean profile_loaded;
};

G_DEFINE_TYPE(GnFollowListItem, gn_follow_list_item, G_TYPE_OBJECT)

enum {
    ITEM_PROP_0,
    ITEM_PROP_PUBKEY,
    ITEM_PROP_RELAY_HINT,
    ITEM_PROP_PETNAME,
    ITEM_PROP_DISPLAY_NAME,
    ITEM_PROP_NIP05,
    ITEM_PROP_PICTURE_URL,
    ITEM_PROP_PROFILE_LOADED,
    ITEM_N_PROPS
};

static GParamSpec *item_props[ITEM_N_PROPS];

static void
gn_follow_list_item_finalize(GObject *object)
{
    GnFollowListItem *self = GN_FOLLOW_LIST_ITEM(object);

    g_free(self->pubkey);
    g_free(self->relay_hint);
    g_free(self->petname);
    g_free(self->display_name);
    g_free(self->nip05);
    g_free(self->picture_url);

    G_OBJECT_CLASS(gn_follow_list_item_parent_class)->finalize(object);
}

static void
gn_follow_list_item_get_property(GObject *object, guint prop_id,
                                  GValue *value, GParamSpec *pspec)
{
    GnFollowListItem *self = GN_FOLLOW_LIST_ITEM(object);

    switch (prop_id) {
    case ITEM_PROP_PUBKEY:
        g_value_set_string(value, self->pubkey);
        break;
    case ITEM_PROP_RELAY_HINT:
        g_value_set_string(value, self->relay_hint);
        break;
    case ITEM_PROP_PETNAME:
        g_value_set_string(value, self->petname);
        break;
    case ITEM_PROP_DISPLAY_NAME:
        g_value_set_string(value, self->display_name);
        break;
    case ITEM_PROP_NIP05:
        g_value_set_string(value, self->nip05);
        break;
    case ITEM_PROP_PICTURE_URL:
        g_value_set_string(value, self->picture_url);
        break;
    case ITEM_PROP_PROFILE_LOADED:
        g_value_set_boolean(value, self->profile_loaded);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
gn_follow_list_item_class_init(GnFollowListItemClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = gn_follow_list_item_finalize;
    object_class->get_property = gn_follow_list_item_get_property;

    item_props[ITEM_PROP_PUBKEY] =
        g_param_spec_string("pubkey", NULL, NULL, NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    item_props[ITEM_PROP_RELAY_HINT] =
        g_param_spec_string("relay-hint", NULL, NULL, NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    item_props[ITEM_PROP_PETNAME] =
        g_param_spec_string("petname", NULL, NULL, NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    item_props[ITEM_PROP_DISPLAY_NAME] =
        g_param_spec_string("display-name", NULL, NULL, NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    item_props[ITEM_PROP_NIP05] =
        g_param_spec_string("nip05", NULL, NULL, NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    item_props[ITEM_PROP_PICTURE_URL] =
        g_param_spec_string("picture-url", NULL, NULL, NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    item_props[ITEM_PROP_PROFILE_LOADED] =
        g_param_spec_boolean("profile-loaded", NULL, NULL, FALSE,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, ITEM_N_PROPS, item_props);
}

static void
gn_follow_list_item_init(GnFollowListItem *self)
{
    (void)self;
}

/* Accessors */
const gchar *gn_follow_list_item_get_pubkey(GnFollowListItem *self)
{
    g_return_val_if_fail(GN_IS_FOLLOW_LIST_ITEM(self), NULL);
    return self->pubkey;
}

const gchar *gn_follow_list_item_get_relay_hint(GnFollowListItem *self)
{
    g_return_val_if_fail(GN_IS_FOLLOW_LIST_ITEM(self), NULL);
    return self->relay_hint;
}

const gchar *gn_follow_list_item_get_petname(GnFollowListItem *self)
{
    g_return_val_if_fail(GN_IS_FOLLOW_LIST_ITEM(self), NULL);
    return self->petname;
}

const gchar *gn_follow_list_item_get_display_name(GnFollowListItem *self)
{
    g_return_val_if_fail(GN_IS_FOLLOW_LIST_ITEM(self), NULL);
    return self->display_name;
}

const gchar *gn_follow_list_item_get_nip05(GnFollowListItem *self)
{
    g_return_val_if_fail(GN_IS_FOLLOW_LIST_ITEM(self), NULL);
    return self->nip05;
}

const gchar *gn_follow_list_item_get_picture_url(GnFollowListItem *self)
{
    g_return_val_if_fail(GN_IS_FOLLOW_LIST_ITEM(self), NULL);
    return self->picture_url;
}

gboolean gn_follow_list_item_get_profile_loaded(GnFollowListItem *self)
{
    g_return_val_if_fail(GN_IS_FOLLOW_LIST_ITEM(self), FALSE);
    return self->profile_loaded;
}

/* Create item from follow entry */
static GnFollowListItem *
gn_follow_list_item_new_from_entry(GnostrFollowEntry *entry)
{
    GnFollowListItem *item = g_object_new(GN_TYPE_FOLLOW_LIST_ITEM, NULL);
    item->pubkey = g_strdup(entry->pubkey_hex);
    item->relay_hint = g_strdup(entry->relay_hint);
    item->petname = g_strdup(entry->petname);
    return item;
}

/* Update item with profile metadata */
static void
gn_follow_list_item_set_profile(GnFollowListItem *self,
                                 const gchar *display_name,
                                 const gchar *nip05,
                                 const gchar *picture_url)
{
    gboolean changed = FALSE;

    if (g_strcmp0(self->display_name, display_name) != 0) {
        g_free(self->display_name);
        self->display_name = g_strdup(display_name);
        changed = TRUE;
    }
    if (g_strcmp0(self->nip05, nip05) != 0) {
        g_free(self->nip05);
        self->nip05 = g_strdup(nip05);
        changed = TRUE;
    }
    if (g_strcmp0(self->picture_url, picture_url) != 0) {
        g_free(self->picture_url);
        self->picture_url = g_strdup(picture_url);
        changed = TRUE;
    }

    if (!self->profile_loaded) {
        self->profile_loaded = TRUE;
        changed = TRUE;
    }

    if (changed) {
        g_object_notify_by_pspec(G_OBJECT(self), item_props[ITEM_PROP_DISPLAY_NAME]);
        g_object_notify_by_pspec(G_OBJECT(self), item_props[ITEM_PROP_NIP05]);
        g_object_notify_by_pspec(G_OBJECT(self), item_props[ITEM_PROP_PICTURE_URL]);
        g_object_notify_by_pspec(G_OBJECT(self), item_props[ITEM_PROP_PROFILE_LOADED]);
    }
}

/* ========== GnFollowListModel ========== */

/* Prefetch buffer size - load profiles this many items ahead of visible range */
#define PROFILE_PREFETCH_BUFFER 10

struct _GnFollowListModel {
    GObject parent_instance;

    /* All loaded items */
    GPtrArray *all_items;      /* GnFollowListItem* */

    /* Filtered view */
    GPtrArray *filtered_items; /* GnFollowListItem* (refs to all_items) */

    /* State */
    gchar *pubkey;             /* User whose follows we're showing */
    gchar *filter_text;
    gboolean is_loading;
    GCancellable *cancellable;

    /* Viewport-aware lazy loading (nostrc-1mzg) */
    guint visible_start;       /* First visible item index */
    guint visible_end;         /* Last visible item index (exclusive) */
    GHashTable *profile_requested; /* pubkey_hex -> TRUE for items with pending/completed profile requests */

    /* Pending async GTask (nostrc-yjl8) */
    GTask *pending_task;       /* Non-NULL while _async load is in progress */
};

static void gn_follow_list_model_list_model_iface_init(GListModelInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GnFollowListModel, gn_follow_list_model, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, gn_follow_list_model_list_model_iface_init))

enum {
    PROP_0,
    PROP_IS_LOADING,
    PROP_PUBKEY,
    PROP_TOTAL_COUNT,
    N_PROPS
};

static GParamSpec *props[N_PROPS];

/* GListModel interface */

static GType
gn_follow_list_model_get_item_type(GListModel *model)
{
    (void)model;
    return GN_TYPE_FOLLOW_LIST_ITEM;
}

static guint
gn_follow_list_model_get_n_items(GListModel *model)
{
    GnFollowListModel *self = GN_FOLLOW_LIST_MODEL(model);
    return self->filtered_items ? self->filtered_items->len : 0;
}

static gpointer
gn_follow_list_model_get_item(GListModel *model, guint position)
{
    GnFollowListModel *self = GN_FOLLOW_LIST_MODEL(model);

    if (!self->filtered_items || position >= self->filtered_items->len)
        return NULL;

    return g_object_ref(g_ptr_array_index(self->filtered_items, position));
}

static void
gn_follow_list_model_list_model_iface_init(GListModelInterface *iface)
{
    iface->get_item_type = gn_follow_list_model_get_item_type;
    iface->get_n_items = gn_follow_list_model_get_n_items;
    iface->get_item = gn_follow_list_model_get_item;
}

/* Filter helpers */

static gboolean
item_matches_filter(GnFollowListItem *item, const gchar *filter)
{
    if (!filter || !*filter) return TRUE;

    gchar *lower_filter = g_utf8_strdown(filter, -1);

    /* Match pubkey */
    if (item->pubkey && g_strstr_len(item->pubkey, -1, lower_filter)) {
        g_free(lower_filter);
        return TRUE;
    }

    /* Match petname */
    if (item->petname) {
        gchar *lower_petname = g_utf8_strdown(item->petname, -1);
        gboolean match = g_strstr_len(lower_petname, -1, lower_filter) != NULL;
        g_free(lower_petname);
        if (match) {
            g_free(lower_filter);
            return TRUE;
        }
    }

    /* Match display name */
    if (item->display_name) {
        gchar *lower_name = g_utf8_strdown(item->display_name, -1);
        gboolean match = g_strstr_len(lower_name, -1, lower_filter) != NULL;
        g_free(lower_name);
        if (match) {
            g_free(lower_filter);
            return TRUE;
        }
    }

    /* Match NIP-05 */
    if (item->nip05) {
        gchar *lower_nip05 = g_utf8_strdown(item->nip05, -1);
        gboolean match = g_strstr_len(lower_nip05, -1, lower_filter) != NULL;
        g_free(lower_nip05);
        if (match) {
            g_free(lower_filter);
            return TRUE;
        }
    }

    g_free(lower_filter);
    return FALSE;
}

static void
apply_filter(GnFollowListModel *self)
{
    guint old_len = self->filtered_items ? self->filtered_items->len : 0;

    if (self->filtered_items) {
        g_ptr_array_unref(self->filtered_items);
    }
    self->filtered_items = g_ptr_array_new();

    if (self->all_items) {
        for (guint i = 0; i < self->all_items->len; i++) {
            GnFollowListItem *item = g_ptr_array_index(self->all_items, i);
            if (item_matches_filter(item, self->filter_text)) {
                g_ptr_array_add(self->filtered_items, item);
            }
        }
    }

    guint new_len = self->filtered_items->len;

    /* Emit items-changed */
    g_list_model_items_changed(G_LIST_MODEL(self), 0, old_len, new_len);
}

/* GObject overrides */

static void
gn_follow_list_model_finalize(GObject *object)
{
    GnFollowListModel *self = GN_FOLLOW_LIST_MODEL(object);

    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);

    /* Cancel pending async task (nostrc-yjl8) */
    if (self->pending_task) {
        g_task_return_new_error(self->pending_task, G_IO_ERROR,
                                G_IO_ERROR_CANCELLED, "Model finalized");
        g_clear_object(&self->pending_task);
    }

    g_clear_pointer(&self->all_items, g_ptr_array_unref);
    g_clear_pointer(&self->filtered_items, g_ptr_array_unref);
    g_clear_pointer(&self->profile_requested, g_hash_table_destroy);
    g_free(self->pubkey);
    g_free(self->filter_text);

    G_OBJECT_CLASS(gn_follow_list_model_parent_class)->finalize(object);
}

static void
gn_follow_list_model_get_property(GObject *object, guint prop_id,
                                   GValue *value, GParamSpec *pspec)
{
    GnFollowListModel *self = GN_FOLLOW_LIST_MODEL(object);

    switch (prop_id) {
    case PROP_IS_LOADING:
        g_value_set_boolean(value, self->is_loading);
        break;
    case PROP_PUBKEY:
        g_value_set_string(value, self->pubkey);
        break;
    case PROP_TOTAL_COUNT:
        g_value_set_uint(value, self->all_items ? self->all_items->len : 0);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
gn_follow_list_model_class_init(GnFollowListModelClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = gn_follow_list_model_finalize;
    object_class->get_property = gn_follow_list_model_get_property;

    props[PROP_IS_LOADING] =
        g_param_spec_boolean("is-loading", NULL, NULL, FALSE,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    props[PROP_PUBKEY] =
        g_param_spec_string("pubkey", NULL, NULL, NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    props[PROP_TOTAL_COUNT] =
        g_param_spec_uint("total-count", NULL, NULL, 0, G_MAXUINT, 0,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, props);
}

static void
gn_follow_list_model_init(GnFollowListModel *self)
{
    self->all_items = g_ptr_array_new_with_free_func(g_object_unref);
    self->filtered_items = g_ptr_array_new();
    self->cancellable = g_cancellable_new();
    self->profile_requested = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    self->visible_start = 0;
    self->visible_end = 0;
}

/* Public API */

GnFollowListModel *
gn_follow_list_model_new(void)
{
    return g_object_new(GN_TYPE_FOLLOW_LIST_MODEL, NULL);
}

void
gn_follow_list_model_clear(GnFollowListModel *self)
{
    g_return_if_fail(GN_IS_FOLLOW_LIST_MODEL(self));

    guint old_len = self->filtered_items ? self->filtered_items->len : 0;

    g_ptr_array_set_size(self->all_items, 0);
    g_ptr_array_set_size(self->filtered_items, 0);

    g_free(self->pubkey);
    self->pubkey = NULL;

    if (old_len > 0) {
        g_list_model_items_changed(G_LIST_MODEL(self), 0, old_len, 0);
    }

    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_PUBKEY]);
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_TOTAL_COUNT]);
}

/* Callback for profile resolution */
static void
on_profile_resolved(const char *pubkey_hex, const GnostrProfileMeta *meta, gpointer user_data)
{
    GnFollowListItem *item = GN_FOLLOW_LIST_ITEM(user_data);
    (void)pubkey_hex;

    if (meta) {
        gn_follow_list_item_set_profile(item, meta->display_name, meta->nip05, meta->picture);
    }

    g_object_unref(item);
}

/* Callback when follow list entries are fetched */
static void
on_follow_list_loaded(GPtrArray *entries, gpointer user_data)
{
    GnFollowListModel *self = GN_FOLLOW_LIST_MODEL(user_data);

    guint old_len = self->filtered_items ? self->filtered_items->len : 0;

    /* Clear existing items and profile request tracking */
    g_ptr_array_set_size(self->all_items, 0);
    g_hash_table_remove_all(self->profile_requested);

    if (entries) {
        /* nostrc-1mzg: Only add items to model, do NOT request profiles here.
         * Profiles will be loaded lazily via set_visible_range() when items
         * become visible in the viewport. This prevents O(n) profile requests
         * for users with thousands of follows. */
        for (guint i = 0; i < entries->len; i++) {
            GnostrFollowEntry *entry = g_ptr_array_index(entries, i);
            GnFollowListItem *item = gn_follow_list_item_new_from_entry(entry);
            g_ptr_array_add(self->all_items, item);
        }
        g_ptr_array_unref(entries);
    }

    /* Apply filter and emit changes */
    apply_filter(self);

    self->is_loading = FALSE;
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_IS_LOADING]);
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_TOTAL_COUNT]);

    /* Request profiles for initially visible items (if range is set) */
    if (self->visible_end > self->visible_start) {
        gn_follow_list_model_set_visible_range(self, self->visible_start, self->visible_end);
    }

    /* Complete pending async task if one exists (nostrc-yjl8) */
    if (self->pending_task) {
        g_task_return_boolean(self->pending_task, TRUE);
        g_clear_object(&self->pending_task);
    }

    (void)old_len;
}

void
gn_follow_list_model_load_for_pubkey(GnFollowListModel *self,
                                      const gchar *pubkey_hex)
{
    g_return_if_fail(GN_IS_FOLLOW_LIST_MODEL(self));
    g_return_if_fail(pubkey_hex != NULL && strlen(pubkey_hex) == 64);

    /* Cancel any pending load */
    g_cancellable_cancel(self->cancellable);
    g_clear_object(&self->cancellable);
    self->cancellable = g_cancellable_new();

    /* Store pubkey */
    g_free(self->pubkey);
    self->pubkey = g_strdup(pubkey_hex);

    self->is_loading = TRUE;
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_IS_LOADING]);
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_PUBKEY]);

    /* Fetch follow list (cache-first, then relay) */
    gnostr_follow_list_fetch_async(pubkey_hex, self->cancellable,
                                    on_follow_list_loaded, self);
}

void
gn_follow_list_model_load_for_pubkey_async(GnFollowListModel *self,
                                            const gchar *pubkey_hex,
                                            GCancellable *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data)
{
    g_return_if_fail(GN_IS_FOLLOW_LIST_MODEL(self));

    /* Store the GTask so on_follow_list_loaded can complete it
     * when the async fetch actually finishes (nostrc-yjl8). */
    g_clear_object(&self->pending_task);
    if (callback) {
        self->pending_task = g_task_new(self, cancellable, callback, user_data);
    }

    /* Start async loading (does NOT block the main thread) */
    gn_follow_list_model_load_for_pubkey(self, pubkey_hex);
}

gboolean
gn_follow_list_model_load_for_pubkey_finish(GnFollowListModel *self,
                                             GAsyncResult *result,
                                             GError **error)
{
    g_return_val_if_fail(GN_IS_FOLLOW_LIST_MODEL(self), FALSE);
    g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
    return g_task_propagate_boolean(G_TASK(result), error);
}

void
gn_follow_list_model_filter(GnFollowListModel *self, const gchar *search_text)
{
    g_return_if_fail(GN_IS_FOLLOW_LIST_MODEL(self));

    g_free(self->filter_text);
    self->filter_text = g_strdup(search_text);

    apply_filter(self);
}

gboolean
gn_follow_list_model_is_loading(GnFollowListModel *self)
{
    g_return_val_if_fail(GN_IS_FOLLOW_LIST_MODEL(self), FALSE);
    return self->is_loading;
}

const gchar *
gn_follow_list_model_get_pubkey(GnFollowListModel *self)
{
    g_return_val_if_fail(GN_IS_FOLLOW_LIST_MODEL(self), NULL);
    return self->pubkey;
}

guint
gn_follow_list_model_get_total_count(GnFollowListModel *self)
{
    g_return_val_if_fail(GN_IS_FOLLOW_LIST_MODEL(self), 0);
    return self->all_items ? self->all_items->len : 0;
}

/* nostrc-1mzg: Request profiles for items in a range that haven't been requested yet */
static void
request_profiles_for_range(GnFollowListModel *self, guint start, guint end)
{
    if (!self->filtered_items || self->filtered_items->len == 0)
        return;

    gpointer profile_service = gnostr_profile_service_get_default();
    if (!profile_service)
        return;

    /* Clamp range to valid indices */
    guint n_items = self->filtered_items->len;
    if (start >= n_items) return;
    if (end > n_items) end = n_items;

    guint requested_count = 0;

    for (guint i = start; i < end; i++) {
        GnFollowListItem *item = g_ptr_array_index(self->filtered_items, i);
        if (!item || !item->pubkey)
            continue;

        /* Skip if already requested */
        if (g_hash_table_contains(self->profile_requested, item->pubkey))
            continue;

        /* Mark as requested and fetch */
        g_hash_table_insert(self->profile_requested, g_strdup(item->pubkey), GINT_TO_POINTER(1));
        gnostr_profile_service_request(
            profile_service,
            item->pubkey,
            on_profile_resolved,
            g_object_ref(item)
        );
        requested_count++;
    }

    if (requested_count > 0) {
        g_debug("[FOLLOW-LIST] Requested %u profiles for range [%u, %u)", requested_count, start, end);
    }
}

void
gn_follow_list_model_set_visible_range(GnFollowListModel *self, guint start, guint end)
{
    g_return_if_fail(GN_IS_FOLLOW_LIST_MODEL(self));

    /* Update stored range */
    self->visible_start = start;
    self->visible_end = end;

    if (start >= end)
        return;

    /* Calculate prefetch range (visible + buffer on both sides) */
    guint prefetch_start = (start > PROFILE_PREFETCH_BUFFER) ? (start - PROFILE_PREFETCH_BUFFER) : 0;
    guint prefetch_end = end + PROFILE_PREFETCH_BUFFER;

    /* Request profiles for visible + prefetch range */
    request_profiles_for_range(self, prefetch_start, prefetch_end);
}

gboolean
gn_follow_list_model_get_visible_range(GnFollowListModel *self, guint *start, guint *end)
{
    g_return_val_if_fail(GN_IS_FOLLOW_LIST_MODEL(self), FALSE);

    if (start) *start = self->visible_start;
    if (end) *end = self->visible_end;

    return (self->visible_end > self->visible_start);
}
