/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-composer.h - NIP-29 message composer shell
 *
 * Text entry + send button. Emits ::send-requested; actual send
 * implementation is deferred to the actions bead (nostrc-oxo).
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GN_NIP29_COMPOSER_H
#define GN_NIP29_COMPOSER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GN_TYPE_NIP29_COMPOSER (gn_nip29_composer_get_type())
G_DECLARE_FINAL_TYPE(GnNip29Composer, gn_nip29_composer,
                     GN, NIP29_COMPOSER, GtkBox)

GnNip29Composer *gn_nip29_composer_new(void);
gchar           *gn_nip29_composer_get_text(GnNip29Composer *self);
void             gn_nip29_composer_clear(GnNip29Composer *self);
void             gn_nip29_composer_set_send_sensitive(GnNip29Composer *self,
                                                      gboolean         sensitive);

G_END_DECLS

#endif /* GN_NIP29_COMPOSER_H */
