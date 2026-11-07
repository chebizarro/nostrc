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

/* Warm the desktop catalog on the GUI main context before rendering rows.
 * Included in unreleased 0.1.0; idempotent and cosmetic only. */
GROTTO_PANELS_API void grotto_panels_app_info_prepare(void);

/* Cosmetic desktop metadata only, never identity/authorization evidence.
 * Uses a process-local indexed desktop catalog, refreshed by GAppInfoMonitor.
 * Returns a newly allocated cosmetic name; label may be a user-assigned name.
 * Callers must display the principal separately; this does not attest a name.
 * If icon is non-NULL, returns an owned desktop icon or generic app fallback.
 * Included in unreleased 0.1.0. No public structure or existing ABI is changed. */
GROTTO_PANELS_API char *grotto_panels_app_display_info(const char *principal, const char *label, GIcon **icon);

/* Only public display metadata. No opaque daemon dictionaries enter a panel.
 * Strings are untrusted plain text; state/reason/access_policy are daemon facts,
 * not inputs to a panel-side permission evaluator. All strings are copied. */
typedef struct _GrottoPanelsMetadata {
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
  gboolean is_default;
  gboolean watch_only;
  gboolean can_sign;
  gboolean backup_acknowledged; /* Owner acknowledgement, not recovery proof. */
  guint app_count;
  const char *principal;
  const char *verified_as;
  const char *transport;
  const char *operation;
  const char *outcome;
  const char *trust;
  const char *decision;
  const char *relay;
  gint event_kind;
  gboolean has_event_kind;
  guint64 timestamp;
  guint64 last_used_at;
  guint64 expires_at;
  guint64 revision;
  guint64 owner_generation;
  gboolean paused;
  gboolean remote_enabled;
  const char *const *relays;
  const char *const *methods;
  /* Host-supplied, descriptive preset scope; never evaluated as authority by GTK. */
  const char *const *effective_permissions;
} GrottoPanelsMetadata;

#define GROTTO_TYPE_PANELS_ITEM (grotto_panels_item_get_type())
GROTTO_PANELS_API
G_DECLARE_FINAL_TYPE(GrottoPanelsItem, grotto_panels_item, GROTTO, PANELS_ITEM, GObject)
GROTTO_PANELS_API GrottoPanelsItem *grotto_panels_item_new(const GrottoPanelsMetadata *metadata, GError **error);
/* Additive constructor: preserves the original metadata struct's ABI. */
GROTTO_PANELS_API GrottoPanelsItem *grotto_panels_item_new_with_backup_banner_state(const GrottoPanelsMetadata *metadata, gboolean dismissed, GError **error);
GROTTO_PANELS_API gboolean grotto_panels_item_get_backup_banner_dismissed(GrottoPanelsItem *self);
GROTTO_PANELS_API gboolean grotto_panels_metadata_validate(const GrottoPanelsMetadata *metadata, GError **error);
/* Admission checks reject known credential encodings/markers, not arbitrary
 * covert data hidden in ordinary text. This is not a sandbox for native code. */
GROTTO_PANELS_API gboolean grotto_panels_public_text_validate(const char *text, GError **error);
/* transfer none; valid for the item's lifetime. Items are immutable; replace a
 * model item to publish new metadata. */
GROTTO_PANELS_API const GrottoPanelsMetadata *grotto_panels_item_get_metadata(GrottoPanelsItem *self);

typedef enum _GrottoPanelsModel {
  GROTTO_PANELS_IDENTITIES,
  GROTTO_PANELS_APPS,
  GROTTO_PANELS_REQUESTS,
  GROTTO_PANELS_ACTIVITY,
  GROTTO_PANELS_STATUS,
  GROTTO_PANELS_N_MODELS
} GrottoPanelsModel;

typedef enum _GrottoPanelCommand {
  GROTTO_PANEL_SET_CLIENT_LABEL,       /* Cosmetic manager operation only. */
  GROTTO_PANEL_SET_DEFAULT_IDENTITY,
  GROTTO_PANEL_SET_CLIENT_TRUST,       /* Tier/preset changes: approver ONLY. */
  GROTTO_PANEL_SET_PERMISSIONS,
  GROTTO_PANEL_REVOKE_CLIENT,
  GROTTO_PANEL_PAUSE,
  GROTTO_PANEL_RESUME,
  GROTTO_PANEL_CLEAR_ACTIVITY,
  GROTTO_PANEL_CONFIGURE_REMOTE,
  GROTTO_PANEL_RESET_REMOTE,
  GROTTO_PANEL_SET_IDENTITY_LABEL,     /* Grotto-local metadata until W6. */
  GROTTO_PANEL_DISMISS_BACKUP_BANNER, /* Host-persisted cosmetic preference only. */
  GROTTO_PANEL_N_COMMANDS
} GrottoPanelCommand;

typedef enum _GrottoPanelsAccess {
  GROTTO_PANELS_ACCESS_SESSION, /* pause only */
  GROTTO_PANELS_ACCESS_MANAGER, /* cosmetic preferences only; reads are models */
  GROTTO_PANELS_ACCESS_APPROVER
} GrottoPanelsAccess;
/* Classification documents the boundary; it never grants authorization. */
GROTTO_PANELS_API GrottoPanelsAccess grotto_panel_command_get_access(GrottoPanelCommand command);
/* Complete, closed command schema. No dictionaries, arbitrary RPC names,
 * object payloads or uninterpreted bytes. Unused members MUST be zero/NULL.
 * Labels are cosmetic; trust is a separate approver-only field/command. */
typedef struct _GrottoPanelPermission {
  const char *operation;
  const char *decision;
  gint event_kind;
  guint64 expires_at;
  const char *relay;
} GrottoPanelPermission;
typedef struct _GrottoPanelCommandArgs {
  const char *principal;
  const char *label;
  const char *public_key;
  const char *trust;
  const GrottoPanelPermission *permissions;
  gsize n_permissions;
  guint64 expected_revision;
  guint64 expected_generation; /* Snapshot's daemon owner; zero for no snapshot. */
  gboolean remote_enabled;
  const char *const *relays;
  const char *const *methods;
} GrottoPanelCommandArgs;
GROTTO_PANELS_API gboolean grotto_panel_command_args_validate(GrottoPanelCommand command,
  const GrottoPanelCommandArgs *args, GError **error);

typedef enum _GrottoPanelsQuery { GROTTO_PANELS_QUERY_PERMISSIONS, GROTTO_PANELS_QUERY_ACTIVITY } GrottoPanelsQuery;

#define GROTTO_TYPE_PANELS_SERVICE (grotto_panels_service_get_type())
GROTTO_PANELS_API
G_DECLARE_INTERFACE(GrottoPanelsService, grotto_panels_service, GROTTO_PANELS, SERVICE, GObject)
struct _GrottoPanelsServiceInterface {
  GTypeInterface parent_iface;
  GListModel *(*get_model)(GrottoPanelsService *self, GrottoPanelsModel model);
  gboolean (*is_available)(GrottoPanelsService *self);
  gboolean (*can)(GrottoPanelsService *self, GrottoPanelCommand command);
  void (*invoke_async)(GrottoPanelsService *self, GrottoPanelCommand command,
                       const GrottoPanelCommandArgs *args, GCancellable *cancellable,
                       GAsyncReadyCallback callback, gpointer user_data);
  gboolean (*invoke_finish)(GrottoPanelsService *self, GAsyncResult *result, GError **error);
  void (*query_async)(GrottoPanelsService *self, GrottoPanelsQuery query,
    const char *principal, guint64 before, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data);
  GListModel *(*query_finish)(GrottoPanelsService *self, GAsyncResult *result, GError **error);
  /* Explicit opt-in, so an older host's can() never receives a new
   * command value that could index beyond its compiled capability table. */
  gboolean (*can_dismiss_backup_banner)(GrottoPanelsService *self);
  gpointer padding[5];
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
  const GrottoPanelCommandArgs *args, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
GROTTO_PANELS_API gboolean grotto_panels_service_invoke_finish(GrottoPanelsService *self, GAsyncResult *result, GError **error);

/* Security callbacks exist only in the trusted approver host. The shared read
 * service never acquires their authority. Results must be boolean GTasks. */
GROTTO_PANELS_API void grotto_panels_service_query_async(GrottoPanelsService *self,
  GrottoPanelsQuery query, const char *principal, guint64 before, GCancellable *cancel,
  GAsyncReadyCallback callback, gpointer data);
GROTTO_PANELS_API GListModel *grotto_panels_service_query_finish(GrottoPanelsService *self,
  GAsyncResult *result, GError **error);

/* Public metadata only. No signed event, file path, credential or transport
 * crosses the panel boundary. Host results are owned GrottoProfileInfo GTasks. */
typedef enum _GrottoProfileField {
  GROTTO_PROFILE_NAME, GROTTO_PROFILE_DISPLAY_NAME, GROTTO_PROFILE_ABOUT,
  GROTTO_PROFILE_PICTURE, GROTTO_PROFILE_BANNER, GROTTO_PROFILE_NIP05,
  GROTTO_PROFILE_LUD16, GROTTO_PROFILE_LUD06, GROTTO_PROFILE_WEBSITE,
  GROTTO_PROFILE_N_FIELDS
} GrottoProfileField;
typedef enum _GrottoProfileAction {
  GROTTO_PROFILE_FETCH, GROTTO_PROFILE_SAVE, GROTTO_PROFILE_CHOOSE_AVATAR,
  GROTTO_PROFILE_CHOOSE_BANNER,
  GROTTO_PROFILE_LOAD_IMAGES, /* Deprecated/unused; retained for ABI compatibility. */
  GROTTO_PROFILE_VERIFY_NIP05,
  GROTTO_PROFILE_FETCH_CACHED /* Cache only: never discovery, HTTP or signing. */
} GrottoProfileAction;
typedef enum _GrottoNip05Status {
  GROTTO_NIP05_UNCHECKED, GROTTO_NIP05_VERIFIED, GROTTO_NIP05_MISMATCH,
  GROTTO_NIP05_INVALID, GROTTO_NIP05_UNAVAILABLE
} GrottoNip05Status;
typedef struct _GrottoProfileInfo {
  char *fields[GROTTO_PROFILE_N_FIELDS];
  GdkTexture *images[2];
  guint64 created_at;
  gboolean published, cached, tor_blocked, bot, no_relays;
  GrottoNip05Status nip05_status;
  char **discovery_relays, **publish_relays, **auth_required_relays, **bootstrap_relays;
  gboolean bootstrap_used;
} GrottoProfileInfo;
GROTTO_PANELS_API GrottoProfileInfo *grotto_profile_info_copy(const GrottoProfileInfo *info);
GROTTO_PANELS_API void grotto_profile_info_free(GrottoProfileInfo *info);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GrottoProfileInfo, grotto_profile_info_free)
GROTTO_PANELS_API gboolean grotto_profile_info_validate(const GrottoProfileInfo *info, GError **error);
/* Main-context-only, bounded public display cache. dup returns an owned copy or
 * NULL. Hosts publish successful reads/refreshes; older profiles are ignored.
 * profile-updated(public_key) is emitted after an effective display change.
 * Views can consult dup before showing loading UI and listen for refreshes. */
GROTTO_PANELS_API GrottoProfileInfo *grotto_panels_service_dup_profile(GrottoPanelsService *self, const char *public_key);
GROTTO_PANELS_API void grotto_panels_service_set_profile(GrottoPanelsService *self, const char *public_key, const GrottoProfileInfo *info);

typedef struct _GrottoPanelsHost {
  void (*show_toast)(gpointer host, const char *message);
  void (*review_request)(gpointer host, const char *request_id);
  void (*add_identity)(gpointer host);
  void (*add_app)(gpointer host);
  void (*open_backup)(gpointer host, const char *public_key);
  void (*remove_identity)(gpointer host, const char *public_key);
  void (*replace_identity)(gpointer host, const char *public_key);
  void (*open_in_signer)(gpointer host, const char *public_target);
  gboolean (*can_command)(gpointer host, GrottoPanelCommand command);
  void (*command_async)(gpointer host, GrottoPanelCommand command,
    const GrottoPanelCommandArgs *args, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data);
  void (*profile_async)(gpointer host, GrottoProfileAction action, const char *public_key,
    const GrottoProfileInfo *info, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data);
} GrottoPanelsHost;

#define GROTTO_TYPE_PANEL (grotto_panel_get_type())
GROTTO_PANELS_API
G_DECLARE_DERIVABLE_TYPE(GrottoPanel, grotto_panel, GROTTO, PANEL, AdwNavigationPage)
struct _GrottoPanelClass {
  AdwNavigationPageClass parent_class;
  gpointer padding[8];
};
/* Abstract base. show-window-controls defaults to TRUE; hosts with their own
 * window chrome set it FALSE (also inherited by pinned Identity subpages).
 * service property is readable/writable; NULL detaches.
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
typedef enum _GrottoPanelHostAction {
  GROTTO_PANEL_HOST_TOAST, GROTTO_PANEL_HOST_REVIEW, GROTTO_PANEL_HOST_ADD_IDENTITY,
  GROTTO_PANEL_HOST_ADD_APP, GROTTO_PANEL_HOST_BACKUP, GROTTO_PANEL_HOST_REMOVE,
  GROTTO_PANEL_HOST_REPLACE, GROTTO_PANEL_HOST_OPEN_SIGNER, GROTTO_PANEL_HOST_N_ACTIONS
} GrottoPanelHostAction;
/* The callback table/data are private; panels cannot bypass admission by calling
 * a retrieved raw callback with a credential-bearing payload. */
GROTTO_PANELS_API gboolean grotto_panel_host_can(GrottoPanel *self, GrottoPanelHostAction action);
GROTTO_PANELS_API gboolean grotto_panel_host_invoke(GrottoPanel *self, GrottoPanelHostAction action,
  const char *public_value, GError **error);
GROTTO_PANELS_API gboolean grotto_panel_can_command(GrottoPanel *self, GrottoPanelCommand command);
GROTTO_PANELS_API void grotto_panel_command_async(GrottoPanel *self, GrottoPanelCommand command,
  const GrottoPanelCommandArgs *args, GAsyncReadyCallback callback, gpointer user_data);
GROTTO_PANELS_API gboolean grotto_panel_command_finish(GrottoPanel *self, GAsyncResult *result, GError **error);
GROTTO_PANELS_API gboolean grotto_panel_has_profile_host(GrottoPanel *self);
GROTTO_PANELS_API void grotto_panel_profile_async(GrottoPanel *self, GrottoProfileAction action,
  const char *public_key, const GrottoProfileInfo *info, GCancellable *cancel,
  GAsyncReadyCallback callback, gpointer data);
GROTTO_PANELS_API GrottoProfileInfo *grotto_panel_profile_finish(GrottoPanel *self,
  GAsyncResult *result, GError **error);
/* Standard plain-text row helper, deliberately no permission interpretation. */
GROTTO_PANELS_API GtkWidget *grotto_panel_create_metadata_row(GrottoPanelsItem *item);

#define GROTTO_TYPE_IDENTITY_PANEL (grotto_identity_panel_get_type())
GROTTO_PANELS_API
G_DECLARE_FINAL_TYPE(GrottoIdentityPanel, grotto_identity_panel, GROTTO, IDENTITY_PANEL, GrottoPanel)
/* NULL public_key displays the default identity; otherwise pin this page. */
GROTTO_PANELS_API GtkWidget *grotto_identity_panel_new(GrottoPanelsService *service, const char *public_key);
GROTTO_PANELS_API AdwNavigationView *grotto_identity_panel_get_navigation(GrottoIdentityPanel *self);

typedef struct _GrottoPanelDescriptor {
  const char *id;
  const char *title;
  const char *icon_name;
} GrottoPanelDescriptor;
GROTTO_PANELS_API const GrottoPanelDescriptor *grotto_panels_get_descriptors(gsize *length);

#define GROTTO_TYPE_APPS_PANEL (grotto_apps_panel_get_type())
GROTTO_PANELS_API
G_DECLARE_FINAL_TYPE(GrottoAppsPanel, grotto_apps_panel, GROTTO, APPS_PANEL, GrottoPanel)
GROTTO_PANELS_API GtkWidget *grotto_apps_panel_new(GrottoPanelsService *service);
GROTTO_PANELS_API AdwNavigationView *grotto_apps_panel_get_navigation(GrottoAppsPanel *self);
/* NULL or empty principal returns to the unfiltered inbox/root page. */
GROTTO_PANELS_API void grotto_apps_panel_show_app(GrottoAppsPanel *self, const char *principal);
GROTTO_PANELS_API void grotto_apps_panel_filter_identity(GrottoAppsPanel *self, const char *public_key);
/* Trusted host inbox, backed by its application-owned approval controller. */
GROTTO_PANELS_API void grotto_apps_panel_set_pending_group(GrottoAppsPanel *self, GtkWidget *group);

#define GROTTO_TYPE_ACTIVITY_PANEL (grotto_activity_panel_get_type())
GROTTO_PANELS_API
G_DECLARE_FINAL_TYPE(GrottoActivityPanel, grotto_activity_panel, GROTTO, ACTIVITY_PANEL, GrottoPanel)
GROTTO_PANELS_API GtkWidget *grotto_activity_panel_new(GrottoPanelsService *service);
GROTTO_PANELS_API void grotto_activity_panel_filter_app(GrottoActivityPanel *self, const char *principal);
#define GROTTO_TYPE_REMOTE_PANEL (grotto_remote_panel_get_type())
GROTTO_PANELS_API
G_DECLARE_FINAL_TYPE(GrottoRemotePanel, grotto_remote_panel, GROTTO, REMOTE_PANEL, GrottoPanel)
GROTTO_PANELS_API GtkWidget *grotto_remote_panel_new(GrottoPanelsService *service);
/* Additive connection display API. The trusted host supplies the shareable
 * bunker URI only, not account/transport keys or policy. NULL clears it.
 * connection-link-requested and show-connection-link ask the host to refresh
 * the display or open its connection QR sheet, respectively. */
GROTTO_PANELS_API void grotto_remote_panel_set_connection_link(GrottoRemotePanel *self, const char *uri);

G_END_DECLS
