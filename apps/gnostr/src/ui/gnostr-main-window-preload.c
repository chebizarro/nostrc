#define G_LOG_DOMAIN "gnostr-main-window-preload"

#include "gnostr-main-window-private.h"

#include <nostr-gobject-1.0/storage_ndb.h>

#include "../util/profile_event_validation.h"

static void
profile_apply_item_free_local(gpointer p)
{
    ProfileApplyCtx *it = (ProfileApplyCtx *)p;
    if (!it)
        return;
    g_free(it->pubkey_hex);
    g_free(it->content_json);
    g_free(it);
}

static void
prepopulate_profiles_thread(GTask *task,
                            gpointer source_object,
                            gpointer task_data G_GNUC_UNUSED,
                            GCancellable *cancellable G_GNUC_UNUSED)
{
    (void)source_object;

    void *txn = NULL;
    char **arr = NULL;
    int n = 0;
    int brc = storage_ndb_begin_query(&txn, NULL);
    if (brc != 0) {
        g_warning("prepopulate_profiles_thread: begin_query failed rc=%d", brc);
        g_task_return_pointer(task, NULL, NULL);
        return;
    }

    const char *filters = "[{\"kinds\":[0]}]";
    int rc = storage_ndb_query(txn, filters, &arr, &n, NULL);
    g_debug("prepopulate_profiles_thread: query rc=%d count=%d", rc, n);

    GPtrArray *items = NULL;
    if (rc == 0 && arr && n > 0) {
        items = g_ptr_array_new_with_free_func(profile_apply_item_free_local);
        for (int i = 0; i < n; i++) {
            const char *evt_json = arr[i];
            if (!evt_json)
                continue;

            g_autofree gchar *pk_hex = NULL;
            g_autofree gchar *content_json = NULL;
            g_autofree gchar *reason = NULL;
            gint64 created_at = 0;
            if (!gnostr_profile_event_extract_for_apply(evt_json, &pk_hex, &content_json, &created_at, &reason)) {
                g_debug("prepopulate_profiles_thread: skipping invalid cached profile event: %s",
                        reason ? reason : "unknown");
                continue;
            }

            ProfileApplyCtx *pctx = g_new0(ProfileApplyCtx, 1);
            pctx->pubkey_hex = g_strdup(pk_hex);
            pctx->content_json = g_strdup(content_json);
            pctx->created_at = created_at;
            g_ptr_array_add(items, pctx);
        }

        if (items->len == 0) {
            g_ptr_array_free(items, TRUE);
            items = NULL;
        }
    }

    storage_ndb_free_results(arr, n);
    storage_ndb_end_query(txn);
    g_task_return_pointer(task, items, NULL);
}

static void
on_prepopulate_profiles_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    (void)user_data;
    GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(source);
    GPtrArray *items = g_task_propagate_pointer(G_TASK(result), NULL);

    if (items && items->len > 0 && GNOSTR_IS_MAIN_WINDOW(self)) {
        g_debug("prepopulate_all_profiles_from_cache: scheduling %u cached profiles", items->len);
        gnostr_main_window_schedule_apply_profiles_internal(self, items);
    } else if (items) {
        g_ptr_array_free(items, TRUE);
    }
}

void
gnostr_main_window_prepopulate_all_profiles_from_cache_internal(GnostrMainWindow *self)
{
    if (!GNOSTR_IS_MAIN_WINDOW(self))
        return;

    GTask *task = g_task_new(self, NULL, on_prepopulate_profiles_done, NULL);
    g_task_run_in_thread(task, prepopulate_profiles_thread);
    g_object_unref(task);
}
