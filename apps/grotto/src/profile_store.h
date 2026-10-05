/* profile_store.h - Nostr profile (kind:0 metadata) management
 *
 * Handles loading, editing, and publishing of Nostr profile metadata.
 * Profile fields per NIP-01/NIP-05/NIP-57:
 * - name: Display name
 * - about: Bio/description
 * - picture: Avatar URL
 * - banner: Banner image URL
 * - nip05: NIP-05 identifier (user@domain.com)
 * - lud16: Lightning address for zaps
 * - website: Personal website URL
 */
#ifndef APPS_GNOSTR_SIGNER_PROFILE_STORE_H
#define APPS_GNOSTR_SIGNER_PROFILE_STORE_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _ProfileStore ProfileStore;

/* Profile data structure */
typedef struct {
  gchar *npub;           /* Public key in bech32 */
  gchar *name;           /* Display name */
  gchar *about;          /* Bio/description */
  gchar *picture;        /* Avatar URL */
  gchar *banner;         /* Banner image URL */
  gchar *nip05;          /* NIP-05 identifier */
  gchar *lud16;          /* Lightning address */
  gchar *website;        /* Website URL */
  gint64 created_at;     /* Event timestamp */
  gboolean dirty;        /* Has unsaved changes */
} NostrProfile;

/* Create a new profile store */
ProfileStore *profile_store_new(void);

/* Free the profile store */
void profile_store_free(ProfileStore *ps);

/* Free a profile */
void nostr_profile_free(NostrProfile *profile);

/* Create a copy of a profile */
NostrProfile *nostr_profile_copy(const NostrProfile *profile);

/* Get profile for an identity (from cache or creates empty) */
NostrProfile *profile_store_get(ProfileStore *ps, const gchar *npub);

/* Update profile locally (marks as dirty, does not publish) */
void profile_store_update(ProfileStore *ps, const NostrProfile *profile);

/* Parse profile from kind:0 event JSON */
NostrProfile *profile_store_parse_event(const gchar *event_json);

/* Build kind:0 event JSON from profile (ready for signing) */
gchar *profile_store_build_event_json(const NostrProfile *profile);

/* Load profile from local cache */
gboolean profile_store_load_cached(ProfileStore *ps, const gchar *npub,
                                   NostrProfile **out_profile);

/* Save profile to local cache */
void profile_store_save_cached(ProfileStore *ps, const NostrProfile *profile);

/* Check if profile has unsaved changes */
gboolean profile_store_is_dirty(ProfileStore *ps, const gchar *npub);

/* Clear dirty flag (after successful publish) */
void profile_store_clear_dirty(ProfileStore *ps, const gchar *npub);

/* Get cache directory path */
const gchar *profile_store_cache_dir(void);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_PROFILE_STORE_H */
