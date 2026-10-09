/* Private: translation macros for portable nostr-gtk sources. */
#pragma once

#include <nostr-gtk-1.0/gn-portable-i18n.h>

#undef GETTEXT_PACKAGE
#define GETTEXT_PACKAGE GN_PORTABLE_GETTEXT_DOMAIN
#include <glib/gi18n-lib.h>
