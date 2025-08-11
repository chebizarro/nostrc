#pragma once
#include <glib.h>

typedef struct _PolicyStore PolicyStore;

typedef struct {
  gchar *app_id;
  gchar *account;
  gboolean decision; /* TRUE=approve, FALSE=deny */
} PolicyEntry;

PolicyStore *policy_store_new(void);
void policy_store_free(PolicyStore *ps);

/* Load from disk (no error if missing). */
void policy_store_load(PolicyStore *ps);
/* Save to disk; best-effort. */
void policy_store_save(PolicyStore *ps);

/* Lookup; returns TRUE if a remembered decision exists. */
gboolean policy_store_get(PolicyStore *ps, const gchar *app_id, const gchar *account, gboolean *out_decision);

/* Set or update decision. */
void policy_store_set(PolicyStore *ps, const gchar *app_id, const gchar *account, gboolean decision);

/* Remove a policy; returns TRUE if removed. */
gboolean policy_store_unset(PolicyStore *ps, const gchar *app_id, const gchar *account);

/* Enumerate all entries; caller owns returned GPtrArray of PolicyEntry* and each entry fields. */
GPtrArray *policy_store_list(PolicyStore *ps);
