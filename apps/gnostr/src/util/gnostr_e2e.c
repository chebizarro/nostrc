#include "gnostr_e2e.h"

#include <stdio.h>
#include <string.h>
#include <glib/gstdio.h>

#include <nostr-gobject-1.0/storage_ndb.h>

gboolean gnostr_e2e_enabled(void) {
  const char *v = g_getenv("GNOSTR_E2E");
  return (v && *v && g_strcmp0(v, "1") == 0);
}

gboolean gnostr_e2e_seed_storage(GError **error) {
  const char *path = g_getenv("GNOSTR_E2E_SEED_JSONL");
  if (!path || !*path) {
    g_set_error(error,
                G_FILE_ERROR,
                G_FILE_ERROR_INVAL,
                "GNOSTR_E2E_SEED_JSONL not set");
    return FALSE;
  }

  gchar *data = NULL;
  gsize len = 0;
  GError *local_err = NULL;
  if (!g_file_get_contents(path, &data, &len, &local_err)) {
    if (local_err) {
      g_propagate_error(error, local_err);
      local_err = NULL;
    } else {
      g_set_error(error,
                  G_FILE_ERROR,
                  G_FILE_ERROR_FAILED,
                  "Failed to read seed file: %s",
                  path);
    }
    return FALSE;
  }

  if (!data || len == 0) {
    g_free(data);
    g_set_error(error,
                G_FILE_ERROR,
                G_FILE_ERROR_INVAL,
                "Seed file is empty: %s",
                path);
    return FALSE;
  }

  int rc = storage_ndb_ingest_ldjson((const char *)data, (size_t)len);
  g_free(data);

  if (rc != 0) {
    g_set_error(error,
                G_FILE_ERROR,
                G_FILE_ERROR_FAILED,
                "storage_ndb_ingest_ldjson failed (rc=%d) for %s",
                rc,
                path);
    return FALSE;
  }

  g_message("e2e: seeded storage from %s (%" G_GSIZE_FORMAT " bytes)", path, len);
  return TRUE;
}

void gnostr_e2e_mark_ready(void) {
  /* Always print marker to stdout for harness detection */
  g_print("GNOSTR_E2E_READY\n");
  fflush(stdout);

  const char *ready_file = g_getenv("GNOSTR_E2E_READY_FILE");
  if (!ready_file || !*ready_file) return;

  /* Ensure parent dir exists */
  gchar *parent = g_path_get_dirname(ready_file);
  if (parent && *parent && g_strcmp0(parent, ".") != 0) {
    (void)g_mkdir_with_parents(parent, 0700);
  }
  g_free(parent);

  GError *err = NULL;
  if (!g_file_set_contents(ready_file, "", 0, &err)) {
    g_warning("e2e: failed to touch ready file %s: %s",
              ready_file,
              err ? err->message : "(unknown)");
    g_clear_error(&err);
  }
}