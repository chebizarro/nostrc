#ifndef GNOSTR_NIP39_IDENTITY_H
#define GNOSTR_NIP39_IDENTITY_H

#include <glib.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

/**
 * NIP-39 External Identity Platforms
 *
 * Supported platforms for identity claims per NIP-39.
 */
typedef enum {
  GNOSTR_NIP39_PLATFORM_UNKNOWN = 0,
  GNOSTR_NIP39_PLATFORM_GITHUB,
  GNOSTR_NIP39_PLATFORM_TWITTER,
  GNOSTR_NIP39_PLATFORM_MASTODON,
  GNOSTR_NIP39_PLATFORM_TELEGRAM,
  GNOSTR_NIP39_PLATFORM_KEYBASE,
  GNOSTR_NIP39_PLATFORM_DNS,
  GNOSTR_NIP39_PLATFORM_REDDIT,
  GNOSTR_NIP39_PLATFORM_WEBSITE
} GnostrNip39Platform;

/**
 * NIP-39 Verification Status
 */
typedef enum {
  GNOSTR_NIP39_STATUS_UNKNOWN = 0,    /* Not yet verified */
  GNOSTR_NIP39_STATUS_VERIFYING,      /* Verification in progress */
  GNOSTR_NIP39_STATUS_VERIFIED,       /* Successfully verified */
  GNOSTR_NIP39_STATUS_FAILED,         /* Verification failed */
  GNOSTR_NIP39_STATUS_UNVERIFIABLE    /* Platform doesn't support verification */
} GnostrNip39Status;

/**
 * GnostrExternalIdentity:
 *
 * Represents a single external identity claim from an "i" tag.
 * Format: ["i", "platform:identity", "proof_url"]
 */
typedef struct {
  GnostrNip39Platform platform;  /* Parsed platform enum */
  char *platform_name;           /* Original platform string (e.g., "github") */
  char *identity;                /* Identity on the platform (e.g., "username") */
  char *proof_url;               /* URL to proof (e.g., gist URL) */
  GnostrNip39Status status;      /* Verification status */
  gint64 verified_at;            /* Unix timestamp when verified (0 if not) */
} GnostrExternalIdentity;

/**
 * Parse an "i" tag into an external identity.
 *
 * @param tag_value The "i" tag value (format: "platform:identity")
 * @param proof_url The proof URL from the tag (nullable)
 * @return Newly allocated identity, or NULL on parse error. Caller must free with gnostr_external_identity_free().
 */
GnostrExternalIdentity *gnostr_nip39_parse_identity(const char *tag_value, const char *proof_url);

/**
 * Parse all "i" tags from a JSON event and return external identities.
 *
 * @param event_json_str The full kind 0 event JSON string
 * @return GPtrArray of GnostrExternalIdentity* (caller owns array and elements). NULL on error.
 */
GPtrArray *gnostr_nip39_parse_identities_from_event(const char *event_json_str);

/**
 * Free an external identity.
 *
 * @param identity The identity to free
 */
void gnostr_external_identity_free(GnostrExternalIdentity *identity);

/**
 * Get the platform enum from a platform string.
 *
 * @param platform_str The platform string (e.g., "github", "twitter")
 * @return The platform enum value
 */
GnostrNip39Platform gnostr_nip39_platform_from_string(const char *platform_str);

/**
 * Get a platform string from the enum.
 *
 * @param platform The platform enum
 * @return Static string (do not free)
 */
const char *gnostr_nip39_platform_to_string(GnostrNip39Platform platform);

/**
 * Get the icon name for a platform.
 *
 * @param platform The platform enum
 * @return Static icon name string (do not free)
 */
const char *gnostr_nip39_get_platform_icon(GnostrNip39Platform platform);

/**
 * Get a display-friendly name for a platform.
 *
 * @param platform The platform enum
 * @return Static display name string (do not free)
 */
const char *gnostr_nip39_get_platform_display_name(GnostrNip39Platform platform);

/**
 * Get the profile URL for an identity on a platform.
 *
 * @param identity The external identity
 * @return Newly allocated URL string, or NULL if not applicable. Caller must free.
 */
char *gnostr_nip39_get_profile_url(const GnostrExternalIdentity *identity);

/**
 * Create a widget row for displaying an external identity.
 *
 * @param identity The external identity to display
 * @return A new GtkWidget containing the identity row
 */
GtkWidget *gnostr_nip39_create_identity_row(const GnostrExternalIdentity *identity);

/**
 * Build "i" tags JSON array from a list of external identities.
 * Used when editing profile to regenerate the event tags.
 *
 * @param identities GPtrArray of GnostrExternalIdentity*
 * @return JSON string of the tags array (caller must free), or NULL on error
 */
char *gnostr_nip39_build_tags_json(GPtrArray *identities);

/**
 * Get verification status string for debugging.
 *
 * @param status The status enum value
 * @return Static string describing the status
 */
const char *gnostr_nip39_status_to_string(GnostrNip39Status status);

G_END_DECLS

#endif /* GNOSTR_NIP39_IDENTITY_H */
