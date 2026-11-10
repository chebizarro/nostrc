#ifndef GH_STORE_REPORTS_H
#define GH_STORE_REPORTS_H
#include "gh-store.h"
G_BEGIN_DECLS
typedef struct {
  gint64 id, created_at;
  gchar *unsigned_json, *signed_json, *targets_json, *outcomes_json, *state;
} GhStoreReport;
void gh_store_report_free(GhStoreReport *report);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhStoreReport, gh_store_report_free)
/* The approved payload and targets are immutable; only delivery state changes. */
gint64 gh_store_report_approve(GhStore *store, const gchar *unsigned_json,
                              const gchar *targets_json, gint64 created_at, GError **error);
GhStoreReport *gh_store_report_load(GhStore *store, gint64 id, GError **error);
GPtrArray *gh_store_report_list(GhStore *store, GError **error);
gboolean gh_store_report_update(GhStore *store, gint64 id, const gchar *signed_json,
                                 const gchar *outcomes_json, const gchar *state, GError **error);
G_END_DECLS
#endif
