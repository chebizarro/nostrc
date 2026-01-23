#pragma once
#include <adwaita.h>
G_BEGIN_DECLS
#define TYPE_IDENTITY_ROW (identity_row_get_type())
#define IS_IDENTITY_ROW(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), TYPE_IDENTITY_ROW))
G_DECLARE_FINAL_TYPE(IdentityRow, identity_row, IDENTITY, ROW, AdwActionRow)
GtkWidget *identity_row_new(void);

/**
 * identity_row_set_identity:
 * @self: an #IdentityRow
 * @label: the display label for this identity
 * @npub: the npub identifier
 * @is_active: whether this is the currently active identity
 *
 * Sets the identity information and updates accessibility labels (nostrc-qfdg).
 */
void identity_row_set_identity(IdentityRow *self, const char *label, const char *npub, gboolean is_active);

G_END_DECLS
