/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip49-keyencrypt-plugin.h - NIP-49 Private Key Encryption Plugin
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef NIP49_KEYENCRYPT_PLUGIN_H
#define NIP49_KEYENCRYPT_PLUGIN_H

#include <glib-object.h>

G_BEGIN_DECLS

#define NIP49_TYPE_KEYENCRYPT_PLUGIN (nip49_keyencrypt_plugin_get_type())
G_DECLARE_FINAL_TYPE(Nip49KeyencryptPlugin, nip49_keyencrypt_plugin, NIP49, KEYENCRYPT_PLUGIN, GObject)

G_END_DECLS

#endif /* NIP49_KEYENCRYPT_PLUGIN_H */
