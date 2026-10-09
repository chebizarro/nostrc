#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _GhDiagnostics GhDiagnostics;

typedef enum {
  GH_DIAGNOSTIC_COMPONENT_RELAY,
  GH_DIAGNOSTIC_COMPONENT_STORE,
  GH_DIAGNOSTIC_COMPONENT_SIGNER,
  GH_DIAGNOSTIC_COMPONENT_NIP17,
  GH_DIAGNOSTIC_COMPONENT_NIP29,
  GH_DIAGNOSTIC_COMPONENT_MARMOT,
  GH_DIAGNOSTIC_COMPONENT_UI,
  GH_DIAGNOSTIC_COMPONENT_N
} GhDiagnosticComponent;

typedef enum {
  GH_DIAGNOSTIC_EVENT_CONNECT_FAILED,
  GH_DIAGNOSTIC_EVENT_PUBLISH_REJECTED,
  GH_DIAGNOSTIC_EVENT_HISTORY_PARTIAL,
  GH_DIAGNOSTIC_EVENT_RENDER_FALLBACK,
  GH_DIAGNOSTIC_EVENT_N
} GhDiagnosticEvent;

typedef enum {
  GH_DIAGNOSTIC_RESULT_OK,
  GH_DIAGNOSTIC_RESULT_RETRY,
  GH_DIAGNOSTIC_RESULT_FAILED,
  GH_DIAGNOSTIC_RESULT_N
} GhDiagnosticResult;

/* state_home is for isolated tests; NULL uses the process's XDG state home. */
GhDiagnostics *gh_diagnostics_new(GSettings *settings, const gchar *state_home);
void gh_diagnostics_free(GhDiagnostics *self);
void gh_diagnostics_record(GhDiagnostics *self, GhDiagnosticComponent component,
                           GhDiagnosticEvent event, GhDiagnosticResult result);
/* Process-owned instance: callers provide enums only, never URLs or GError text. */
GhDiagnostics *gh_diagnostics_get_default(void);
/* Called once by process service startup/teardown. */
void gh_diagnostics_set_default(GhDiagnostics *self);
void gh_diagnostics_record_default(GhDiagnosticComponent component,
                                   GhDiagnosticEvent event, GhDiagnosticResult result);
/* Caller owns a sanitized, immutable snapshot, capped to 8,000 UTF-8 bytes. */
gchar *gh_diagnostics_snapshot(GhDiagnostics *self);
/* Clears in-memory and disk aggregates. A failed delete is reported, never hidden. */
gboolean gh_diagnostics_clear(GhDiagnostics *self, GError **error);
/* NULL unless disk persistence failed in this process. */
const gchar *gh_diagnostics_get_save_error(GhDiagnostics *self);
/* Most recent disable-time deletion error, if any. */
const gchar *gh_diagnostics_get_delete_error(GhDiagnostics *self);
typedef void (*GhDiagnosticsStatusFunc)(GhDiagnostics *self, gpointer data);
void gh_diagnostics_set_status_callback(GhDiagnostics *self,
                                        GhDiagnosticsStatusFunc callback, gpointer data);

G_END_DECLS
