#ifndef GNOSTR_PROFILE_EDIT_H
#define GNOSTR_PROFILE_EDIT_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_PROFILE_EDIT (gnostr_profile_edit_get_type())
G_DECLARE_FINAL_TYPE(GnostrProfileEdit, gnostr_profile_edit, GNOSTR, PROFILE_EDIT, GtkWindow)

/**
 * gnostr_profile_edit_new:
 * @parent: (nullable): parent window for transient-for
 *
 * Create a new profile edit dialog.
 *
 * Returns: (transfer full): a new GnostrProfileEdit instance
 */
GnostrProfileEdit *gnostr_profile_edit_new(GtkWindow *parent);

/**
 * gnostr_profile_edit_set_profile_json:
 * @self: the profile edit dialog
 * @profile_json: JSON string containing current profile metadata
 *
 * Populate the form fields with data from existing kind 0 content.
 */
void gnostr_profile_edit_set_profile_json(GnostrProfileEdit *self, const char *profile_json);

/**
 * gnostr_profile_edit_get_profile_json:
 * @self: the profile edit dialog
 *
 * Serialize the form fields to a JSON string suitable for kind 0 content.
 *
 * Returns: (transfer full): newly allocated JSON string, caller must free
 */
char *gnostr_profile_edit_get_profile_json(GnostrProfileEdit *self);

/**
 * gnostr_profile_edit_set_event_json:
 * @self: the profile edit dialog
 * @event_json: Full kind 0 event JSON (for accessing "i" tags)
 *
 * Set the full event JSON to extract external identities.
 * This should be called after set_profile_json to populate identity UI.
 */
void gnostr_profile_edit_set_event_json(GnostrProfileEdit *self, const char *event_json);

/**
 * gnostr_profile_edit_get_identity_tags_json:
 * @self: the profile edit dialog
 *
 * Get the "i" tags JSON array for external identities.
 *
 * Returns: (transfer full): JSON array string of "i" tags, caller must free
 */
char *gnostr_profile_edit_get_identity_tags_json(GnostrProfileEdit *self);

G_END_DECLS

#endif /* GNOSTR_PROFILE_EDIT_H */
