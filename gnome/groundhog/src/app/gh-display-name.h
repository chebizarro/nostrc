#pragma once
#include <glib-object.h>

G_BEGIN_DECLS

/* People's names across the UI (W33, owner: "if a user's kind 0 metadata is
 * available, use their display name instead of their npub, everywhere").
 * The app sets one resolver (the contact directory's cached, cleaned kind-0
 * names; nothing is fetched by asking); everything that shows a person asks
 * here and falls back to the abbreviated npub. */
typedef const gchar *(*GhDisplayNameFunc)(const gchar *pubkey_hex, gpointer data);
void gh_display_name_set_resolver(GhDisplayNameFunc func, gpointer data);

/* The cached name of pubkey_hex (64 hex), or NULL. Borrowed until the next
 * "changed" for that key. */
const gchar *gh_display_name_lookup(const gchar *pubkey_hex);
/* The name, else "npub1abcde…wxyz". Transfer full. */
gchar *gh_display_name_for(const gchar *pubkey_hex);
/* "npub1abcde…wxyz" (or the full npub when short); the input when not a key. */
gchar *gh_display_name_short_npub(const gchar *pubkey_hex);

/* Emits "changed" (s: pubkey hex) when a name arrives or changes: the app
 * calls gh_display_name_changed() from the directory's "profile-changed". */
GObject *gh_display_name_get_notifier(void);
void gh_display_name_changed(const gchar *pubkey_hex);

G_END_DECLS
