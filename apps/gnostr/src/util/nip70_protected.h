/**
 * @file nip70_protected.h
 * @brief NIP-70 Protected Events implementation
 *
 * NIP-70 defines the "-" tag to mark events that should only be published
 * to specific relays. Protected events MUST NOT be rebroadcast by relays
 * that receive them.
 *
 * When an event has a "-" tag, it indicates:
 * - The event is meant only for the specific relay it was published to
 * - Relays SHOULD NOT rebroadcast the event to other relays
 * - Clients SHOULD warn users when reposting protected events
 * - Clients SHOULD provide UI to mark notes as protected
 *
 * Tag format: ["-"]
 */

#ifndef GNOSTR_NIP70_PROTECTED_H
#define GNOSTR_NIP70_PROTECTED_H

#include <glib.h>
#include <gtk/gtk.h>
#include <stdbool.h>

G_BEGIN_DECLS

/**
 * Protection status for an event
 */
typedef enum {
  GNOSTR_PROTECTED_STATUS_UNKNOWN = 0,  /* Not yet checked */
  GNOSTR_PROTECTED_STATUS_UNPROTECTED,  /* No "-" tag present */
  GNOSTR_PROTECTED_STATUS_PROTECTED,    /* Has "-" tag - protected event */
} GnostrProtectedStatus;

/**
 * Result of protection check operations
 */
typedef struct {
  GnostrProtectedStatus status;
  char *event_id;      /* Event ID (hex) if available */
  char *relay_hint;    /* Relay URL hint if available */
} GnostrProtectedResult;

/**
 * gnostr_nip70_check_event:
 * @event_json: JSON string of the Nostr event
 *
 * Checks if an event has the "-" protection tag.
 *
 * Returns: TRUE if event is protected (has "-" tag)
 */
gboolean gnostr_nip70_check_event(const char *event_json);

/**
 * gnostr_nip70_check_tags_json:
 * @tags_json: JSON array string of event tags
 *
 * Checks if a tags array contains the "-" protection tag.
 *
 * Returns: TRUE if tags include protection marker
 */
gboolean gnostr_nip70_check_tags_json(const char *tags_json);

/**
 * gnostr_nip70_add_protection_tag:
 * @tags_json: JSON array string of existing event tags
 *
 * Adds the "-" protection tag to an existing tags array.
 * If the tag is already present, returns a copy of the original.
 *
 * Returns: (transfer full): New JSON tags array with protection tag added
 */
char *gnostr_nip70_add_protection_tag(const char *tags_json);

/**
 * gnostr_nip70_remove_protection_tag:
 * @tags_json: JSON array string of existing event tags
 *
 * Removes the "-" protection tag from a tags array if present.
 *
 * Returns: (transfer full): New JSON tags array with protection tag removed
 */
char *gnostr_nip70_remove_protection_tag(const char *tags_json);

/**
 * gnostr_nip70_build_protection_tag:
 *
 * Creates a protection tag as a JSON array element.
 * Returns: ["-"] as JSON string
 *
 * Returns: (transfer full): JSON string for protection tag
 */
char *gnostr_nip70_build_protection_tag(void);

/**
 * gnostr_nip70_can_rebroadcast:
 * @event_json: JSON string of the Nostr event
 *
 * Checks if an event can be safely rebroadcast.
 * Protected events should NOT be rebroadcast.
 *
 * Returns: TRUE if event can be rebroadcast, FALSE if protected
 */
gboolean gnostr_nip70_can_rebroadcast(const char *event_json);

/**
 * gnostr_nip70_should_warn_repost:
 * @event_json: JSON string of the Nostr event
 *
 * Checks if a warning should be shown before reposting.
 * Protected events should trigger a warning dialog.
 *
 * Returns: TRUE if a warning should be displayed
 */
gboolean gnostr_nip70_should_warn_repost(const char *event_json);

/* --- UI Widget Helpers --- */

/**
 * gnostr_nip70_create_protected_badge:
 *
 * Creates a GTK widget showing a "Protected" indicator.
 * Suitable for display in note cards.
 *
 * Returns: (transfer floating): A new GtkWidget with the protected badge
 */
GtkWidget *gnostr_nip70_create_protected_badge(void);

/**
 * gnostr_nip70_create_protected_badge_with_tooltip:
 * @tooltip: Custom tooltip text (NULL for default)
 *
 * Creates a protected badge widget with custom tooltip.
 *
 * Returns: (transfer floating): A new GtkWidget with the protected badge
 */
GtkWidget *gnostr_nip70_create_protected_badge_with_tooltip(const char *tooltip);

/**
 * gnostr_nip70_show_repost_warning_dialog:
 * @parent: Parent window for the dialog
 * @event_id_hex: Event ID being reposted (for display)
 * @callback: Callback when user makes a choice
 * @user_data: User data for callback
 *
 * Shows a warning dialog when attempting to repost a protected event.
 * User can choose to proceed or cancel.
 */
typedef void (*GnostrNip70WarningCallback)(gboolean proceed, gpointer user_data);

void gnostr_nip70_show_repost_warning_dialog(GtkWindow *parent,
                                              const char *event_id_hex,
                                              GnostrNip70WarningCallback callback,
                                              gpointer user_data);

/* --- Result Helpers --- */

/**
 * gnostr_nip70_result_new:
 *
 * Creates a new protected result structure.
 *
 * Returns: (transfer full): New result, free with gnostr_nip70_result_free()
 */
GnostrProtectedResult *gnostr_nip70_result_new(void);

/**
 * gnostr_nip70_result_free:
 * @result: Result to free
 *
 * Frees a protected result structure.
 */
void gnostr_nip70_result_free(GnostrProtectedResult *result);

/**
 * gnostr_nip70_status_to_string:
 * @status: Protection status enum value
 *
 * Gets a human-readable string for a protection status.
 *
 * Returns: Static string describing the status
 */
const char *gnostr_nip70_status_to_string(GnostrProtectedStatus status);

/* --- Composer Integration --- */

/**
 * gnostr_nip70_create_protection_toggle:
 *
 * Creates a toggle button for the composer to mark notes as protected.
 * Button shows lock icon and "Protected" label.
 *
 * Returns: (transfer floating): A new GtkToggleButton
 */
GtkWidget *gnostr_nip70_create_protection_toggle(void);

/**
 * gnostr_nip70_get_toggle_state:
 * @toggle: Toggle button created by gnostr_nip70_create_protection_toggle()
 *
 * Gets the current state of a protection toggle.
 *
 * Returns: TRUE if protection is enabled
 */
gboolean gnostr_nip70_get_toggle_state(GtkWidget *toggle);

/**
 * gnostr_nip70_set_toggle_state:
 * @toggle: Toggle button created by gnostr_nip70_create_protection_toggle()
 * @protected: Whether protection should be enabled
 *
 * Sets the state of a protection toggle.
 */
void gnostr_nip70_set_toggle_state(GtkWidget *toggle, gboolean protected);

G_END_DECLS

#endif /* GNOSTR_NIP70_PROTECTED_H */
