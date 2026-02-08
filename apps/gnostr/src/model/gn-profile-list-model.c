/**
 * GnProfileListModel - A GListModel for nostrdb-cached profiles
 *
 * Queries nostrdb for kind:0 (profile metadata) events and provides
 * them as a GListModel for use with GtkListView.
 */

#include "gn-profile-list-model.h"
#include "../storage_ndb.h"
#include "../util/gnostr-profile-service.h"
#include "gn-nostr-profile.h"
#include "nostr_json.h"
#include <json.h>
#include <string.h>
#include <stdio.h>

/* Maximum profiles to load - set high to get all cached profiles.
 * nostrc-v6sx: Increased from 500 to 50000 to show all nostrdb-cached profiles. */
#define PROFILE_LOAD_LIMIT 50000
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

    /* Sort by display_name first, then name, then pubkey as fallback */
    const char *name_a = gn_nostr_profile_get_display_name(ea->profile);
    const char *name_b = gn_nostr_profile_get_display_name(eb->profile);

    /* Fall back to name if display_name is empty or null */
    if (!name_a || *name_a == '\0')
        name_a = gn_nostr_profile_get_name(ea->profile);
    if (!name_b || *name_b == '\0')
        name_b = gn_nostr_profile_get_name(eb->profile);

    /* Fall back to pubkey if both display_name and name are empty */
    if (!name_a || *name_a == '\0')
        name_a = gn_nostr_profile_get_pubkey(ea->profile);
    if (!name_b || *name_b == '\0')
        name_b = gn_nostr_profile_get_pubkey(eb->profile);

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

    /* nostrc-bzoj: Cancel any pending profile fetch callbacks that use this model */
    gpointer svc = gnostr_profile_service_get_default();
    if (svc) {
        guint cancelled = gnostr_profile_service_cancel_for_user_data(svc, self);
        if (cancelled > 0) {
            g_debug("profile-list-model: Cancelled %u pending fetch callbacks on finalize", cancelled);
        }
    }

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

/* nostrc-3nj: Parse profile from kind:0 event JSON using NostrJsonInterface */
static ProfileEntry *
parse_profile_from_event_json(const char *json_str, int json_len)
{
    if (!json_str || json_len <= 0) return NULL;

    /* Ensure NUL-terminated string for the interface */
    char *json_copy = g_strndup(json_str, json_len);
    if (!json_copy) return NULL;

    /* Get pubkey */
    char *pubkey = NULL;
    pubkey = gnostr_json_get_string(json_copy, "pubkey", NULL);
    if (!pubkey) {
        g_free(json_copy);
        return NULL;
    }

    /* Get created_at */
    int64_t created_at = 0;
    created_at = gnostr_json_get_int64(json_copy, "created_at", NULL);

    /* Get content (profile metadata JSON) */
    char *content = NULL;
    content = gnostr_json_get_string(json_copy, "content", NULL);

    /* Create profile */
    GnNostrProfile *profile = gn_nostr_profile_new(pubkey);
    if (content && *content) {
        gn_nostr_profile_update_from_json(profile, content);
    }

    ProfileEntry *entry = g_new0(ProfileEntry, 1);
    entry->profile = profile;
    entry->created_at = (gint64)created_at;
    entry->is_following = FALSE;

    free(pubkey);
    free(content);
    g_free(json_copy);
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
        /* Free any remaining entries (in case of error before transfer) */
        for (guint i = 0; i < data->loaded_profiles->len; i++) {
            profile_entry_free(g_ptr_array_index(data->loaded_profiles, i));
        }
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
    /* Don't use a free function - ownership will be transferred to all_profiles */
    data->loaded_profiles = g_ptr_array_new();

    /* Query nostrdb for all kind:0 events */
    void *txn = NULL;
    int rc = storage_ndb_begin_query_retry(&txn, 5, 10);
    if (rc != 0 || !txn) {
        g_warning("profile-list-model: Failed to begin nostrdb query for profiles");
        g_task_return_pointer(task, data, (GDestroyNotify)load_profiles_data_free);
        return;
    }

    /* Query for kind:0 events with limit */
    char filter_json[128];
    snprintf(filter_json, sizeof(filter_json), "{\"kinds\":[0],\"limit\":%d}", PROFILE_LOAD_LIMIT);

    char **results = NULL;
    int result_count = 0;

    g_message("profile-list-model: querying nostrdb with filter: %s", filter_json);
    rc = storage_ndb_query(txn, filter_json, &results, &result_count);
    g_message("profile-list-model: query returned rc=%d, result_count=%d", rc, result_count);

    if (rc == 0 && results && result_count > 0) {
        for (int i = 0; i < result_count; i++) {
            if (results[i]) {
                ProfileEntry *entry = parse_profile_from_event_json(results[i], strlen(results[i]));
                if (entry) {
                    g_ptr_array_add(data->loaded_profiles, entry);
                }
            }
        }
        g_message("profile-list-model: parsed %u profiles from %d results", data->loaded_profiles->len, result_count);
        storage_ndb_free_results(results, result_count);
    } else {
        g_warning("profile-list-model: query failed or returned no results (rc=%d, count=%d)", rc, result_count);
    }

    storage_ndb_end_query(txn);

    g_task_return_pointer(task, data, (GDestroyNotify)load_profiles_data_free);
}

static void
load_profiles_complete(GObject *source, GAsyncResult *result, gpointer user_data)
{
    (void)user_data;

    /* Get self from source_object where GTask holds a reference */
    GnProfileListModel *self = GN_PROFILE_LIST_MODEL(source);
    if (!GN_IS_PROFILE_LIST_MODEL(self)) return;

    GError *error = NULL;
    LoadProfilesData *data = g_task_propagate_pointer(G_TASK(result), &error);

    if (error) {
        g_warning("profile-list-model: Failed to load profiles: %s", error->message);
        g_error_free(error);
    }

    if (data && data->loaded_profiles) {
        guint old_len = self->all_profiles->len;
        g_message("profile-list-model: load complete - %u profiles loaded", data->loaded_profiles->len);

        /* Clear old profiles */
        g_ptr_array_set_size(self->all_profiles, 0);

        /* Hash table to deduplicate by pubkey */
        GHashTable *seen_pubkeys = g_hash_table_new(g_str_hash, g_str_equal);

        /* Transfer loaded profiles, deduplicating by pubkey */
        for (guint i = 0; i < data->loaded_profiles->len; i++) {
            ProfileEntry *entry = g_ptr_array_index(data->loaded_profiles, i);

            /* Update following and muted status */
            const char *pubkey = gn_nostr_profile_get_pubkey(entry->profile);

            /* Skip duplicate pubkeys - keep the first (most recent) one */
            if (pubkey && g_hash_table_contains(seen_pubkeys, pubkey)) {
                profile_entry_free(entry);
                continue;
            }

            if (pubkey) {
                g_hash_table_add(seen_pubkeys, (gpointer)pubkey);
                if (g_hash_table_contains(self->following_set, pubkey)) {
                    entry->is_following = TRUE;
                }
                if (g_hash_table_contains(self->muted_set, pubkey)) {
                    entry->is_muted = TRUE;
                }
            }

            g_ptr_array_add(self->all_profiles, entry);
        }

        g_hash_table_destroy(seen_pubkeys);

        /* Clear the loaded array - entries are now owned by all_profiles */
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

    /* Pass self as source_object so GTask holds a reference and we can safely
     * access it in the callback even if the caller drops their reference */
    GTask *task = g_task_new(self, NULL, load_profiles_complete, NULL);
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

/* nostrc-bzoj: Check if we have a profile for a pubkey in all_profiles */
static gboolean
has_profile_for_pubkey(GnProfileListModel *self, const char *pubkey)
{
    if (!pubkey) return FALSE;
    for (guint i = 0; i < self->all_profiles->len; i++) {
        ProfileEntry *entry = g_ptr_array_index(self->all_profiles, i);
        const char *pk = gn_nostr_profile_get_pubkey(entry->profile);
        if (pk && g_strcmp0(pk, pubkey) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

/* nostrc-bzoj: Helper to escape a string for JSON */
static char *
json_escape_string(const char *str)
{
    if (!str) return NULL;
    GString *out = g_string_new("");
    for (const char *p = str; *p; p++) {
        switch (*p) {
            case '"':  g_string_append(out, "\\\""); break;
            case '\\': g_string_append(out, "\\\\"); break;
            case '\n': g_string_append(out, "\\n"); break;
            case '\r': g_string_append(out, "\\r"); break;
            case '\t': g_string_append(out, "\\t"); break;
            default:   g_string_append_c(out, *p); break;
        }
    }
    return g_string_free(out, FALSE);
}

/* nostrc-bzoj: Callback when a missing profile is fetched from network */
static void
on_missing_profile_fetched(const char *pubkey_hex,
                           const GnostrProfileMeta *meta,
                           gpointer user_data)
{
    GnProfileListModel *self = GN_PROFILE_LIST_MODEL(user_data);
    if (!GN_IS_PROFILE_LIST_MODEL(self)) return;

    /* Profile not found on network - nothing to add */
    if (!meta) {
        g_debug("profile-list-model: No profile found for followed user %s", pubkey_hex);
        return;
    }

    /* Check we don't already have this profile (race condition check) */
    if (has_profile_for_pubkey(self, pubkey_hex)) {
        return;
    }

    /* Build JSON from meta fields for update_from_json */
    GString *json = g_string_new("{");
    gboolean first = TRUE;

    if (meta->display_name) {
        char *escaped = json_escape_string(meta->display_name);
        g_string_append_printf(json, "%s\"display_name\":\"%s\"",
                               first ? "" : ",", escaped);
        g_free(escaped);
        first = FALSE;
    }
    if (meta->name) {
        char *escaped = json_escape_string(meta->name);
        g_string_append_printf(json, "%s\"name\":\"%s\"",
                               first ? "" : ",", escaped);
        g_free(escaped);
        first = FALSE;
    }
    if (meta->picture) {
        char *escaped = json_escape_string(meta->picture);
        g_string_append_printf(json, "%s\"picture\":\"%s\"",
                               first ? "" : ",", escaped);
        g_free(escaped);
        first = FALSE;
    }
    if (meta->nip05) {
        char *escaped = json_escape_string(meta->nip05);
        g_string_append_printf(json, "%s\"nip05\":\"%s\"",
                               first ? "" : ",", escaped);
        g_free(escaped);
        first = FALSE;
    }
    if (meta->lud16) {
        char *escaped = json_escape_string(meta->lud16);
        g_string_append_printf(json, "%s\"lud16\":\"%s\"",
                               first ? "" : ",", escaped);
        g_free(escaped);
        first = FALSE;
    }
    g_string_append_c(json, '}');

    /* Create a profile entry from the fetched metadata */
    GnNostrProfile *profile = gn_nostr_profile_new(pubkey_hex);
    gn_nostr_profile_update_from_json(profile, json->str);
    g_string_free(json, TRUE);

    ProfileEntry *entry = g_new0(ProfileEntry, 1);
    entry->profile = profile;
    entry->created_at = meta->created_at > 0 ? meta->created_at :
                        (gint64)(g_get_real_time() / G_USEC_PER_SEC);
    entry->is_following = g_hash_table_contains(self->following_set, pubkey_hex);
    entry->is_muted = g_hash_table_contains(self->muted_set, pubkey_hex);

    /* Add to all_profiles */
    g_ptr_array_add(self->all_profiles, entry);
    self->total_count = self->all_profiles->len;

    g_debug("profile-list-model: Added network-fetched profile for %s (is_following=%d)",
            pubkey_hex, entry->is_following);

    /* Rebuild filtered list to show the new profile */
    rebuild_filtered_list(self);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_TOTAL_COUNT]);
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

    /* nostrc-bzoj: Request profiles for followed users we don't have locally.
     * The profile service batches these requests with 150ms debounce. */
    if (pubkeys) {
        gpointer svc = gnostr_profile_service_get_default();
        guint missing_count = 0;

        for (const char **p = pubkeys; *p; p++) {
            const char *pk = *p;
            if (strlen(pk) != 64) continue;  /* Skip invalid pubkeys */

            if (!has_profile_for_pubkey(self, pk)) {
                /* Request this profile from network */
                gnostr_profile_service_request(svc, pk,
                                               on_missing_profile_fetched,
                                               self);
                missing_count++;
            }
        }

        if (missing_count > 0) {
            g_message("profile-list-model: Requested %u missing profiles for followed users",
                      missing_count);
        }
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
