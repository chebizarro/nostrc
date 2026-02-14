#ifndef APPS_GNOSTR_UTIL_GNOSTR_E2E_H
#define APPS_GNOSTR_UTIL_GNOSTR_E2E_H

#include <glib.h>

/* Returns TRUE if GNOSTR_E2E=1 is set */
gboolean gnostr_e2e_enabled(void);

/* Seed storage from GNOSTR_E2E_SEED_JSONL file. Call after storage_ndb_init(). Returns TRUE on success. */
gboolean gnostr_e2e_seed_storage(GError **error);

/* Signal readiness: prints "GNOSTR_E2E_READY" to stdout and touches GNOSTR_E2E_READY_FILE if set */
void gnostr_e2e_mark_ready(void);
#endif /* APPS_GNOSTR_UTIL_GNOSTR_E2E_H */
