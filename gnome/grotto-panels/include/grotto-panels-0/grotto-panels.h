/* SPDX-License-Identifier: MIT */
#pragma once
#include <adwaita.h>
#include "grotto-panels-version.h"

#if defined(_WIN32)
# ifdef GROTTO_PANELS_COMPILATION
#  define GROTTO_PANELS_API __declspec(dllexport)
# else
#  define GROTTO_PANELS_API __declspec(dllimport)
# endif
#else
# define GROTTO_PANELS_API __attribute__((visibility("default")))
#endif

G_BEGIN_DECLS

/* Only public display metadata. No opaque daemon dictionaries enter a panel.
 * Strings are untrusted plain text; state/reason/access_policy are daemon facts,
 * not inputs to a panel-side permission evaluator. All strings are copied. */
typedef struct {
  const char *id;
  const char *label;
  const char *public_key;
  const char *state;
  const char *reason;
  const char *access_policy;
  gboolean needs_review;
  gboolean blocked;
  gint64 deadline_usec; /* Absolute monotonic deadline, never renewed on mount. */
  guint64 sequence;    /* Activity cursor, NOT a snapshot revision. */
} GrottoPanelsMetadata;

#define GROTTO_TYPE_PANELS_ITEM (grotto_panels_item_get_type())
GROTTO_PANELS_API
G_DECLARE_FINAL_TYPE(GrottoPanelsItem, grotto_panels_item, GROTTO, PANELS_ITEM, GObject)
GROTTO_PANELS_API GrottoPanelsItem *grotto_panels_item_new(const GrottoPanelsMetadata *metadata);
/* transfer none; valid for the item's lifetime. Items are immutable; replace a
 * model item to publish new metadata. */
GROTTO_PANELS_API const GrottoPanelsMetadata *grotto_panels_item_get_metadata(GrottoPanelsItem *self);

typedef enum {
  GROTTO_PANELS_IDENTITIES,
  GROTTO_PANELS_APPS,
  GROTTO_PANELS_REQUESTS,
  GROTTO_PANELS_ACTIVITY,
  GROTTO_PANELS_N_MODELS
} GrottoPanelsModel;

typedef enum {
  GROTTO_PANEL_SET_CLIENT_LABEL,       /* (ss): principal, local label */
  GROTTO_PANEL_SET_DEFAULT_IDENTITY,   /* (s): public identity */
  GROTTO_PANEL_SET_CLIENT_TRUST,       /* (ss): principal, daemon preset */
  GROTTO_PANEL_SET_PERMISSIONS,        /* (ssaa{sv}t): daemon contract */
  GROTTO_PANEL_REVOKE_CLIENT,          /* (s): principal */
  GROTTO_PANEL_PAUSE,                  /* (): pause, distinct from resume */
  GROTTO_PANEL_RESUME,                 /* (): approver-only */
  GROTTO_PANEL_CLEAR_ACTIVITY,         /* () */
  GROTTO_PANEL_CONFIGURE_REMOTE,       /* (basas): enabled, relays, methods */
  GROTTO_PANEL_RESET_REMOTE,           /* () */
  GROTTO_PANEL_N_COMMANDS
} GrottoPanelCommand;

typedef enum {
  GROTTO_PANELS_ACCESS_SESSION, /* pause only */
  GROTTO_PANELS_ACCESS_MANAGER, /* cosmetic label only; reads are models */
  GROTTO_PANELS_ACCESS_APPROVER
} GrottoPanelsAccess;
/* Classification documents the boundary; it never grants authorization. */
GROTTO_PANELS_API GrottoPanelsAccess grotto_panel_command_get_access(GrottoPanelCommand command);
GROTTO_PANELS_API const GVariantType *grotto_panel_command_get_args_type(GrottoPanelCommand command);

#define GROTTO_TYPE_PANELS_SERVICE (grotto_panels_service_get_type())
GROTTO_PANELS_API
G_DECLARE_INTERFACE(GrottoPanelsService, grotto_panels_service, GROTTO_PANELS, SERVICE, GObject)
struct _GrottoPanelsServiceInterface {
  GTypeInterface parent_iface;
  GListModel *(*get_model)(GrottoPanelsService *self, GrottoPanelsModel model);
  gboolean (*is_available)(GrottoPanelsService *self);
  gboolean (*can)(GrottoPanelsService *self, GrottoPanelCommand command);
  void (*invoke_async)(GrottoPanelsService *self, GrottoPanelCommand command,
                       GVariant *args, GCancellable *cancellable,
                       GAsyncReadyCallback callback, gpointer user_data);
  gboolean (*invoke_finish)(GrottoPanelsService *self, GAsyncResult *result, GError **error);
  gpointer padding[8];
};
/* All entrypoints, signals and model notifications use the GTK main context.
 * Models: transfer none, item type GrottoPanelsItem, lifetime of service.
 * Optional vfuncs fail closed. Signals: capabilities-changed and
 * availability-changed (no arguments). Capabilities are UI hints, not authority.
 * Async completions must reject stale owner generations/snapshot revisions. */
GROTTO_PANELS_API GListModel *grotto_panels_service_get_model(GrottoPanelsService *self, GrottoPanelsModel model);
GROTTO_PANELS_API gboolean grotto_panels_service_is_available(GrottoPanelsService *self);
GROTTO_PANELS_API gboolean grotto_panels_service_can(GrottoPanelsService *self, GrottoPanelCommand command);
GROTTO_PANELS_API void grotto_panels_service_invoke_async(GrottoPanelsService *self, GrottoPanelCommand command,
  GVariant *args, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
GROTTO_PANELS_API gboolean grotto_panels_service_invoke_finish(GrottoPanelsService *self, GAsyncResult *result, GError **error);

typedef struct {
  void (*show_toast)(gpointer host, const char *message);
  void (*review_request)(gpointer host, const char *request_id);
  void (*add_identity)(gpointer host);
  void (*add_app)(gpointer host);
  void (*open_backup)(gpointer host, const char *public_key);
  void (*remove_identity)(gpointer host, const char *public_key);
  void (*replace_identity)(gpointer host, const char *public_key);
  void (*open_in_signer)(gpointer host, const char *public_target);
} GrottoPanelsHost;

#define GROTTO_TYPE_PANEL (grotto_panel_get_type())
GROTTO_PANELS_API
G_DECLARE_DERIVABLE_TYPE(GrottoPanel, grotto_panel, GROTTO, PANEL, AdwNavigationPage)
struct _GrottoPanelClass {
  AdwNavigationPageClass parent_class;
  gpointer padding[8];
};
/* Abstract base. service property is readable/writable; NULL detaches.
 * service-changed is emitted after attachment, availability or capabilities.
 * Detaching disconnects and cancels the old view's cancellable, not the service.
 * A panel owns a strong reference; the host owns long-lived pending state. */
GROTTO_PANELS_API void grotto_panel_set_service(GrottoPanel *self, GrottoPanelsService *service);
GROTTO_PANELS_API GrottoPanelsService *grotto_panel_get_service(GrottoPanel *self);
GROTTO_PANELS_API GCancellable *grotto_panel_get_cancellable(GrottoPanel *self);
/* Copies callbacks. A NULL callback means hide the action. Destroy called once
 * on replacement/disposal; user_data never travels to the daemon. */
GROTTO_PANELS_API void grotto_panel_set_host(GrottoPanel *self, const GrottoPanelsHost *host,
                                           gpointer user_data, GDestroyNotify destroy);
GROTTO_PANELS_API const GrottoPanelsHost *grotto_panel_get_host(GrottoPanel *self, gpointer *user_data);
/* Standard plain-text row helper, deliberately no permission interpretation. */
GROTTO_PANELS_API GtkWidget *grotto_panel_create_metadata_row(GrottoPanelsItem *item);

G_END_DECLS
