/**
 * GnProfileListModel - A GListModel for nostrdb-cached profiles
 *
 * Queries nostrdb for kind:0 (profile metadata) events and provides
 * them as a GListModel for use with GtkListView.
 */

#include "gn-profile-list-model.h"
#include "../storage_ndb.h"
#include "gn-nostr-profile.h"
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdio.h>

/* Maximum profiles to load at once */
#define PROFILE_LOAD_LIMIT 500
#define PROFILE_BATCH_SIZE 50

/* Profile entry for internal storage */
typedef struct {
    GnNostrProfile *profile;
    gint64 created_at;       /* Timestamp of kind:0 event */
    gboolean is_following;   /* Whether current user follows this profile */
    gboolean is_muted;       /* Whether this profile is muted (NIP-51) */
} ProfileEntry;

static void profile_entry_free(ProfileEntry *entry) {
    if (entry) {
        g_clear_object(&entry->profile);
        g_free(entry);
    }
}

struct _GnProfileListModel {
    GObject parent_instance;

    /* All loaded profiles */
    GPtrArray *all_profiles;     /* ProfileEntry* */

    /* Filtered/sorted view */
    GPtrArray *filtered_profiles; /* ProfileEntry* (references, not owned) */

    /* State */
    GnProfileSortMode sort_mode;
    char *filter_text;
    GHashTable *following_set;   /* pubkey hex -> GINT_TO_POINTER(1) */
    GHashTable *muted_set;       /* pubkey hex -> GINT_TO_POINTER(1) */
    GHashTable *blocked_set;     /* pubkey hex -> GINT_TO_POINTER(1) */
    gboolean is_loading;
    guint total_count;
};

static void gn_profile_list_model_list_model_iface_init(GListModelInterface *iface);

G_DEFINE_TYPE_WITH_CODE(GnProfileListModel, gn_profile_list_model, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, gn_profile_list_model_list_model_iface_init))

enum {
    PROP_0,
    PROP_IS_LOADING,
    PROP_TOTAL_COUNT,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

/* GListModel interface implementation */

static GType
gn_profile_list_model_get_item_type(GListModel *model)
{
    (void)model;
    return GN_TYPE_NOSTR_PROFILE;
}

static guint
gn_profile_list_model_get_n_items(GListModel *model)
{
    GnProfileListModel *self = GN_PROFILE_LIST_MODEL(model);
    return self->filtered_profiles ? self->filtered_profiles->len : 0;
}

static gpointer
gn_profile_list_model_get_item(GListModel *model, guint position)
{
    GnProfileListModel *self = GN_PROFILE_LIST_MODEL(model);

    if (!self->filtered_profiles || position >= self->filtered_profiles->len)
        return NULL;

    ProfileEntry *entry = g_ptr_array_index(self->filtered_profiles, position);
    return g_object_ref(entry->profile);
}

static void
gn_profile_list_model_list_model_iface_init(GListModelInterface *iface)
{
    iface->get_item_type = gn_profile_list_model_get_item_type;
    iface->get_n_items = gn_profile_list_model_get_n_items;
    iface->get_item = gn_profile_list_model_get_item;
}

/* Sorting comparators */

static gint
compare_by_recent(gconstpointer a, gconstpointer b)
{
    const ProfileEntry *ea = *(const ProfileEntry **)a;
    const ProfileEntry *eb = *(const ProfileEntry **)b;

    /* Sort by created_at descending (newest first) */
    if (ea->created_at > eb->created_at) return -1;
    if (ea->created_at < eb->created_at) return 1;
    return 0;
}

static gint
compare_by_alphabetical(gconstpointer a, gconstpointer b)
{
    const ProfileEntry *ea = *(const ProfileEntry **)a;
    const ProfileEntry *eb = *(const ProfileEntry **)b;

    const char *name_a = gn_nostr_profile_get_display_name(ea->profile);
    const char *name_b = gn_nostr_profile_get_display_name(eb->profile);

    if (!name_a) name_a = gn_nostr_profile_get_pubkey(ea->profile);
    if (!name_b) name_b = gn_nostr_profile_get_pubkey(eb->profile);

    return g_utf8_collate(name_a ? name_a : "", name_b ? name_b : "");
}

static gint
compare_by_following(gconstpointer a, gconstpointer b, gpointer user_data)
{
    const ProfileEntry *ea = *(const ProfileEntry **)a;
    const ProfileEntry *eb = *(const ProfileEntry **)b;

    (void)user_data;

    /* Following profiles come first */
    if (ea->is_following && !eb->is_following) return -1;
    if (!ea->is_following && eb->is_following) return 1;

    /* Then sort by name within each group */
    return compare_by_alphabetical(a, b);
}

static void
apply_sort(GnProfileListModel *self)
{
    if (!self->filtered_profiles || self->filtered_profiles->len == 0)
        return;

    switch (self->sort_mode) {
        case GN_PROFILE_SORT_RECENT:
            g_ptr_array_sort(self->filtered_profiles, compare_by_recent);
            break;
        case GN_PROFILE_SORT_ALPHABETICAL:
            g_ptr_array_sort(self->filtered_profiles, compare_by_alphabetical);
            break;
        case GN_PROFILE_SORT_FOLLOWING:
            g_ptr_array_sort_with_data(self->filtered_profiles, compare_by_following, self);
            break;
    }
}

static gboolean
profile_matches_filter(ProfileEntry *entry, const char *filter_text)
{
    if (!filter_text || !*filter_text)
        return TRUE;

    GnNostrProfile *profile = entry->profile;

    /* Case-insensitive search in name, display_name, nip05, about */
    const char *name = gn_nostr_profile_get_name(profile);
    const char *display_name = gn_nostr_profile_get_display_name(profile);
    const char *nip05 = gn_nostr_profile_get_nip05(profile);
    const char *about = gn_nostr_profile_get_about(profile);
    const char *pubkey = gn_nostr_profile_get_pubkey(profile);

    char *filter_lower = g_utf8_strdown(filter_text, -1);
    gboolean matches = FALSE;

    if (name) {
        char *name_lower = g_utf8_strdown(name, -1);
        if (strstr(name_lower, filter_lower)) matches = TRUE;
        g_free(name_lower);
    }

    if (!matches && display_name) {
        char *dn_lower = g_utf8_strdown(display_name, -1);
        if (strstr(dn_lower, filter_lower)) matches = TRUE;
        g_free(dn_lower);
    }

    if (!matches && nip05) {
        char *nip05_lower = g_utf8_strdown(nip05, -1);
        if (strstr(nip05_lower, filter_lower)) matches = TRUE;
        g_free(nip05_lower);
    }

    if (!matches && about) {
        char *about_lower = g_utf8_strdown(about, -1);
        if (strstr(about_lower, filter_lower)) matches = TRUE;
        g_free(about_lower);
    }

    if (!matches && pubkey) {
        /* Also match on pubkey prefix */
        if (g_str_has_prefix(pubkey, filter_text)) matches = TRUE;
    }

    g_free(filter_lower);
    return matches;
}

static void
rebuild_filtered_list(GnProfileListModel *self)
{
    guint old_len = self->filtered_profiles ? self->filtered_profiles->len : 0;

    /* Clear filtered list */
    if (self->filtered_profiles) {
        g_ptr_array_set_size(self->filtered_profiles, 0);
    } else {
        self->filtered_profiles = g_ptr_array_new();
    }

    /* Build filtered list - exclude blocked profiles */
    for (guint i = 0; i < self->all_profiles->len; i++) {
        ProfileEntry *entry = g_ptr_array_index(self->all_profiles, i);
        const char *pubkey = gn_nostr_profile_get_pubkey(entry->profile);

        /* Skip blocked profiles entirely */
        if (pubkey && g_hash_table_contains(self->blocked_set, pubkey)) {
            continue;
        }

        if (profile_matches_filter(entry, self->filter_text)) {
            g_ptr_array_add(self->filtered_profiles, entry);
        }
    }

    /* Apply sorting */
    apply_sort(self);

    /* Emit items-changed */
    guint new_len = self->filtered_profiles->len;
    g_list_model_items_changed(G_LIST_MODEL(self), 0, old_len, new_len);
}

/* GObject implementation */

static void
gn_profile_list_model_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
    GnProfileListModel *self = GN_PROFILE_LIST_MODEL(object);

    switch (prop_id) {
        case PROP_IS_LOADING:
            g_value_set_boolean(value, self->is_loading);
            break;
        case PROP_TOTAL_COUNT:
            g_value_set_uint(value, self->total_count);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
gn_profile_list_model_finalize(GObject *object)
{
    GnProfileListModel *self = GN_PROFILE_LIST_MODEL(object);

    if (self->all_profiles) {
        g_ptr_array_free(self->all_profiles, TRUE);
    }
    if (self->filtered_profiles) {
        g_ptr_array_free(self->filtered_profiles, TRUE);
    }
    if (self->following_set) {
        g_hash_table_destroy(self->following_set);
    }
    if (self->muted_set) {
        g_hash_table_destroy(self->muted_set);
    }
    if (self->blocked_set) {
        g_hash_table_destroy(self->blocked_set);
    }
    g_free(self->filter_text);

    G_OBJECT_CLASS(gn_profile_list_model_parent_class)->finalize(object);
}

static void
gn_profile_list_model_class_init(GnProfileListModelClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->get_property = gn_profile_list_model_get_property;
    object_class->finalize = gn_profile_list_model_finalize;

    properties[PROP_IS_LOADING] = g_param_spec_boolean(
        "is-loading", "Is Loading", "Whether profiles are being loaded",
        FALSE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    properties[PROP_TOTAL_COUNT] = g_param_spec_uint(
        "total-count", "Total Count", "Total profiles in database",
        0, G_MAXUINT, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
gn_profile_list_model_init(GnProfileListModel *self)
{
    self->all_profiles = g_ptr_array_new_with_free_func((GDestroyNotify)profile_entry_free);
    self->filtered_profiles = g_ptr_array_new();
    self->following_set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    self->muted_set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    self->blocked_set = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    self->sort_mode = GN_PROFILE_SORT_RECENT;
    self->filter_text = NULL;
    self->is_loading = FALSE;
    self->total_count = 0;
}

GnProfileListModel *
gn_profile_list_model_new(void)
{
    return g_object_new(GN_TYPE_PROFILE_LIST_MODEL, NULL);
}

/* Parse profile from kind:0 event JSON */
static ProfileEntry *
parse_profile_from_event_json(const char *json, int json_len)
{
    if (!json || json_len <= 0) return NULL;

    GError *error = NULL;
    JsonParser *parser = json_parser_new();

    if (!json_parser_load_from_data(parser, json, json_len, &error)) {
        g_warning("Failed to parse profile event JSON: %s", error->message);
        g_error_free(error);
        g_object_unref(parser);
        return NULL;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return NULL;
    }

    JsonObject *event = json_node_get_object(root);

    /* Get pubkey */
    const char *pubkey = NULL;
    if (json_object_has_member(event, "pubkey")) {
        pubkey = json_object_get_string_member(event, "pubkey");
    }
    if (!pubkey) {
        g_object_unref(parser);
        return NULL;
    }

    /* Get created_at */
    gint64 created_at = 0;
    if (json_object_has_member(event, "created_at")) {
        created_at = json_object_get_int_member(event, "created_at");
    }

    /* Get content (profile metadata JSON) */
    const char *content = NULL;
    if (json_object_has_member(event, "content")) {
        content = json_object_get_string_member(event, "content");
    }

    /* Create profile */
    GnNostrProfile *profile = gn_nostr_profile_new(pubkey);
    if (content && *content) {
        gn_nostr_profile_update_from_json(profile, content);
    }

    ProfileEntry *entry = g_new0(ProfileEntry, 1);
    entry->profile = profile;
    entry->created_at = created_at;
    entry->is_following = FALSE;

    g_object_unref(parser);
    return entry;
}

/* Background loading task */
typedef struct {
    GnProfileListModel *model;
    GPtrArray *loaded_profiles;
} LoadProfilesData;

static void
load_profiles_data_free(LoadProfilesData *data)
{
    if (data->loaded_profiles) {
        g_ptr_array_free(data->loaded_profiles, TRUE);
    }
    g_free(data);
}

static void
load_profiles_in_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable)
{
    (void)source_object;
    (void)task_data;
    (void)cancellable;

    LoadProfilesData *data = g_new0(LoadProfilesData, 1);
    data->loaded_profiles = g_ptr_array_new_with_free_func((GDestroyNotify)profile_entry_free);

    /* Query nostrdb for all kind:0 events */
    void *txn = NULL;
    int rc = storage_ndb_begin_query_retry(&txn, 5, 10);
    if (rc != 0 || !txn) {
        g_warning("Failed to begin nostrdb query for profiles");
        g_task_return_pointer(task, data, (GDestroyNotify)load_profiles_data_free);
        return;
    }

    /* Query for kind:0 events with limit */
    char filter_json[128];
    snprintf(filter_json, sizeof(filter_json), "{\"kinds\":[0],\"limit\":%d}", PROFILE_LOAD_LIMIT);

    char **results = NULL;
    int result_count = 0;

    rc = storage_ndb_query(txn, filter_json, &results, &result_count);
    if (rc == 0 && results && result_count > 0) {
        for (int i = 0; i < result_count; i++) {
            if (results[i]) {
                ProfileEntry *entry = parse_profile_from_event_json(results[i], strlen(results[i]));
                if (entry) {
                    g_ptr_array_add(data->loaded_profiles, entry);
                }
            }
        }
        storage_ndb_free_results(results, result_count);
    }

    storage_ndb_end_query(txn);

    g_task_return_pointer(task, data, (GDestroyNotify)load_profiles_data_free);
}

static void
load_profiles_complete(GObject *source, GAsyncResult *result, gpointer user_data)
{
    (void)source;
    GnProfileListModel *self = GN_PROFILE_LIST_MODEL(user_data);

    GError *error = NULL;
    LoadProfilesData *data = g_task_propagate_pointer(G_TASK(result), &error);

    if (error) {
        g_warning("Failed to load profiles: %s", error->message);
        g_error_free(error);
    }

    if (data && data->loaded_profiles) {
        guint old_len = self->all_profiles->len;

        /* Clear old profiles */
        g_ptr_array_set_size(self->all_profiles, 0);

        /* Transfer loaded profiles */
        for (guint i = 0; i < data->loaded_profiles->len; i++) {
            ProfileEntry *entry = g_ptr_array_index(data->loaded_profiles, i);

            /* Update following and muted status */
            const char *pubkey = gn_nostr_profile_get_pubkey(entry->profile);
            if (pubkey) {
                if (g_hash_table_contains(self->following_set, pubkey)) {
                    entry->is_following = TRUE;
                }
                if (g_hash_table_contains(self->muted_set, pubkey)) {
                    entry->is_muted = TRUE;
                }
            }

            g_ptr_array_add(self->all_profiles, entry);
        }

        /* Clear the loaded array without freeing entries (we transferred ownership) */
        g_ptr_array_set_size(data->loaded_profiles, 0);

        self->total_count = self->all_profiles->len;

        /* Rebuild filtered list */
        rebuild_filtered_list(self);

        g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_TOTAL_COUNT]);

        (void)old_len;
    }

    /* Mark loading complete */
    self->is_loading = FALSE;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_IS_LOADING]);

    if (data) {
        load_profiles_data_free(data);
    }
}

void
gn_profile_list_model_load_profiles(GnProfileListModel *self)
{
    g_return_if_fail(GN_IS_PROFILE_LIST_MODEL(self));

    if (self->is_loading)
        return;

    self->is_loading = TRUE;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_IS_LOADING]);

    GTask *task = g_task_new(NULL, NULL, load_profiles_complete, self);
    g_task_run_in_thread(task, load_profiles_in_thread);
    g_object_unref(task);
}

void
gn_profile_list_model_filter(GnProfileListModel *self, const char *search_text)
{
    g_return_if_fail(GN_IS_PROFILE_LIST_MODEL(self));

    g_free(self->filter_text);
    self->filter_text = g_strdup(search_text);

    rebuild_filtered_list(self);
}

void
gn_profile_list_model_set_sort_mode(GnProfileListModel *self, GnProfileSortMode mode)
{
    g_return_if_fail(GN_IS_PROFILE_LIST_MODEL(self));

    if (self->sort_mode == mode)
        return;

    self->sort_mode = mode;

    /* Re-sort and notify */
    guint len = self->filtered_profiles ? self->filtered_profiles->len : 0;
    if (len > 0) {
        apply_sort(self);
        g_list_model_items_changed(G_LIST_MODEL(self), 0, len, len);
    }
}

GnProfileSortMode
gn_profile_list_model_get_sort_mode(GnProfileListModel *self)
{
    g_return_val_if_fail(GN_IS_PROFILE_LIST_MODEL(self), GN_PROFILE_SORT_RECENT);
    return self->sort_mode;
}

void
gn_profile_list_model_set_following_set(GnProfileListModel *self, const char **pubkeys)
{
    g_return_if_fail(GN_IS_PROFILE_LIST_MODEL(self));

    /* Clear existing set */
    g_hash_table_remove_all(self->following_set);

    /* Add new pubkeys */
    if (pubkeys) {
        for (const char **p = pubkeys; *p; p++) {
            g_hash_table_insert(self->following_set, g_strdup(*p), GINT_TO_POINTER(1));
        }
    }

    /* Update following status on all entries */
    for (guint i = 0; i < self->all_profiles->len; i++) {
        ProfileEntry *entry = g_ptr_array_index(self->all_profiles, i);
        const char *pubkey = gn_nostr_profile_get_pubkey(entry->profile);
        entry->is_following = pubkey && g_hash_table_contains(self->following_set, pubkey);
    }

    /* If sorting by following, re-sort */
    if (self->sort_mode == GN_PROFILE_SORT_FOLLOWING) {
        guint len = self->filtered_profiles ? self->filtered_profiles->len : 0;
        if (len > 0) {
            apply_sort(self);
            g_list_model_items_changed(G_LIST_MODEL(self), 0, len, len);
        }
    }
}

void
gn_profile_list_model_set_muted_set(GnProfileListModel *self, const char **pubkeys)
{
    g_return_if_fail(GN_IS_PROFILE_LIST_MODEL(self));

    /* Clear existing set */
    g_hash_table_remove_all(self->muted_set);

    /* Add new pubkeys */
    if (pubkeys) {
        for (const char **p = pubkeys; *p; p++) {
            g_hash_table_insert(self->muted_set, g_strdup(*p), GINT_TO_POINTER(1));
        }
    }

    /* Update muted status on all entries */
    for (guint i = 0; i < self->all_profiles->len; i++) {
        ProfileEntry *entry = g_ptr_array_index(self->all_profiles, i);
        const char *pubkey = gn_nostr_profile_get_pubkey(entry->profile);
        entry->is_muted = pubkey && g_hash_table_contains(self->muted_set, pubkey);
    }

    /* Rebuild to update display */
    rebuild_filtered_list(self);
}

void
gn_profile_list_model_set_blocked_set(GnProfileListModel *self, const char **pubkeys)
{
    g_return_if_fail(GN_IS_PROFILE_LIST_MODEL(self));

    /* Clear existing set */
    g_hash_table_remove_all(self->blocked_set);

    /* Add new pubkeys */
    if (pubkeys) {
        for (const char **p = pubkeys; *p; p++) {
            g_hash_table_insert(self->blocked_set, g_strdup(*p), GINT_TO_POINTER(1));
        }
    }

    /* Rebuild to filter out blocked profiles */
    rebuild_filtered_list(self);
}

gboolean
gn_profile_list_model_is_pubkey_muted(GnProfileListModel *self, const char *pubkey)
{
    g_return_val_if_fail(GN_IS_PROFILE_LIST_MODEL(self), FALSE);
    if (!pubkey) return FALSE;
    return g_hash_table_contains(self->muted_set, pubkey);
}

gboolean
gn_profile_list_model_is_loading(GnProfileListModel *self)
{
    g_return_val_if_fail(GN_IS_PROFILE_LIST_MODEL(self), FALSE);
    return self->is_loading;
}

guint
gn_profile_list_model_get_total_count(GnProfileListModel *self)
{
    g_return_val_if_fail(GN_IS_PROFILE_LIST_MODEL(self), 0);
    return self->total_count;
}
