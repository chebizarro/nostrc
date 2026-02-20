/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-group-composer.h - Group message composer widget
 *
 * Text entry with a send button for composing group messages.
 * Emits ::send-requested when the user sends a message.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_GROUP_COMPOSER_H
#define GN_GROUP_COMPOSER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GN_TYPE_GROUP_COMPOSER (gn_group_composer_get_type())
G_DECLARE_FINAL_TYPE(GnGroupComposer, gn_group_composer,
                     GN, GROUP_COMPOSER, GtkBox)

/**
 * gn_group_composer_new:
 *
 * Returns: (transfer full): A new #GnGroupComposer
 */
GnGroupComposer *gn_group_composer_new(void);

/**
 * gn_group_composer_get_text:
 * @self: The composer
 *
 * Returns: (transfer full): The current message text (caller frees)
 */
gchar *gn_group_composer_get_text(GnGroupComposer *self);

/**
 * gn_group_composer_clear:
 * @self: The composer
 *
 * Clear the message text.
 */
void gn_group_composer_clear(GnGroupComposer *self);

/**
 * gn_group_composer_set_sensitive:
 * @self: The composer
 * @sensitive: Whether the send button is sensitive
 */
void gn_group_composer_set_send_sensitive(GnGroupComposer *self,
                                           gboolean         sensitive);

G_END_DECLS

#endif /* GN_GROUP_COMPOSER_H */
