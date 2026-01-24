/*
 * nip58_badges.h - NIP-58 Badge implementation for GNostr
 *
 * NIP-58 defines three event kinds for badges:
 *   - Kind 30009: Badge Definition (created by issuer)
 *   - Kind 8: Badge Award (issuer awards to user)
 *   - Kind 30008: Profile Badges (user displays earned badges)
 */

#ifndef NIP58_BADGES_H
#define NIP58_BADGES_H

#include <glib-object.h>
#include <gio/gio.h>
#include <gdk/gdk.h>

G_BEGIN_DECLS

/* Nostr event kinds for NIP-58 badges */
#define NIP58_KIND_BADGE_AWARD      8
#define NIP58_KIND_PROFILE_BADGES   30008
#define NIP58_KIND_BADGE_DEFINITION 30009

/**
 * GnostrBadgeDefinition:
 *
 * Represents a badge definition (kind 30009).
 * Contains the badge metadata from the issuer.
 */
typedef struct _GnostrBadgeDefinition {
  gchar *identifier;     /* "d" tag value - unique identifier */
  gchar *name;           /* "name" tag - display name */
  gchar *description;    /* "description" tag - badge description */
  gchar *image_url;      /* "image" tag - badge image URL */
  gchar *thumb_url;      /* "thumb" tag - thumbnail URL (optional) */
  gchar *issuer_pubkey;  /* Event author - issuer's pubkey (hex) */
  gchar *event_id;       /* Event ID of the definition */
  gint64 created_at;     /* Creation timestamp */
} GnostrBadgeDefinition;

/**
 * GnostrBadgeAward:
 *
 * Represents a badge award (kind 8).
 * Links a badge definition to awardees.
 */
typedef struct _GnostrBadgeAward {
  gchar *event_id;         /* Event ID of the award */
  gchar *badge_ref;        /* "a" tag referencing badge definition */
  gchar *issuer_pubkey;    /* Event author - issuer's pubkey (hex) */
  GPtrArray *awardees;     /* Array of gchar* pubkeys ("p" tags) */
  gint64 created_at;       /* Award timestamp */
} GnostrBadgeAward;

/**
 * GnostrProfileBadge:
 *
 * A badge displayed on a user's profile (from kind 30008).
 * Contains both the definition and award reference.
 */
typedef struct _GnostrProfileBadge {
  GnostrBadgeDefinition *definition;  /* Badge definition (owned) */
  gchar *award_event_id;              /* Reference to award event */
  gint64 position;                    /* Display order in profile */
} GnostrProfileBadge;

/* ============== Badge Definition API ============== */

/**
 * gnostr_badge_definition_new:
 *
 * Creates a new empty badge definition.
 *
 * Returns: (transfer full): A new badge definition. Free with gnostr_badge_definition_free().
 */
GnostrBadgeDefinition *gnostr_badge_definition_new(void);

/**
 * gnostr_badge_definition_free:
 * @def: Badge definition to free.
 *
 * Frees a badge definition and all its contents.
 */
void gnostr_badge_definition_free(GnostrBadgeDefinition *def);

/**
 * gnostr_badge_definition_parse:
 * @event_json: JSON string of a kind 30009 event.
 *
 * Parses a badge definition from event JSON.
 *
 * Returns: (transfer full): Parsed definition or NULL on error.
 */
GnostrBadgeDefinition *gnostr_badge_definition_parse(const gchar *event_json);

/**
 * gnostr_badge_definition_get_naddr:
 * @def: Badge definition.
 *
 * Builds the NIP-33 address tag value for this definition.
 * Format: "30009:<pubkey>:<identifier>"
 *
 * Returns: (transfer full): Address string (caller frees with g_free).
 */
gchar *gnostr_badge_definition_get_naddr(const GnostrBadgeDefinition *def);

/* ============== Badge Award API ============== */

/**
 * gnostr_badge_award_new:
 *
 * Creates a new empty badge award.
 *
 * Returns: (transfer full): A new badge award. Free with gnostr_badge_award_free().
 */
GnostrBadgeAward *gnostr_badge_award_new(void);

/**
 * gnostr_badge_award_free:
 * @award: Badge award to free.
 *
 * Frees a badge award and all its contents.
 */
void gnostr_badge_award_free(GnostrBadgeAward *award);

/**
 * gnostr_badge_award_parse:
 * @event_json: JSON string of a kind 8 event.
 *
 * Parses a badge award from event JSON.
 *
 * Returns: (transfer full): Parsed award or NULL on error.
 */
GnostrBadgeAward *gnostr_badge_award_parse(const gchar *event_json);

/* ============== Profile Badge API ============== */

/**
 * gnostr_profile_badge_new:
 *
 * Creates a new empty profile badge.
 *
 * Returns: (transfer full): A new profile badge. Free with gnostr_profile_badge_free().
 */
GnostrProfileBadge *gnostr_profile_badge_new(void);

/**
 * gnostr_profile_badge_free:
 * @badge: Profile badge to free.
 *
 * Frees a profile badge and its definition.
 */
void gnostr_profile_badge_free(GnostrProfileBadge *badge);

/**
 * gnostr_profile_badges_parse:
 * @event_json: JSON string of a kind 30008 event.
 *
 * Parses profile badges from a kind 30008 event.
 * Note: This only parses the references; definitions must be fetched separately.
 *
 * Returns: (transfer full) (element-type GnostrProfileBadge): Array of profile badges.
 */
GPtrArray *gnostr_profile_badges_parse(const gchar *event_json);

/* ============== Async Fetch API ============== */

/**
 * GnostrBadgeFetchCallback:
 * @badges: (element-type GnostrProfileBadge): Array of fetched badges.
 * @user_data: User data passed to the fetch function.
 *
 * Callback for badge fetch operations.
 */
typedef void (*GnostrBadgeFetchCallback)(GPtrArray *badges, gpointer user_data);

/**
 * gnostr_fetch_profile_badges_async:
 * @pubkey_hex: User's public key in hex (64 chars).
 * @cancellable: (nullable): Cancellable for the operation.
 * @callback: Callback when badges are fetched.
 * @user_data: User data for callback.
 *
 * Fetches a user's profile badges (kind 30008), then resolves the
 * badge definitions (kind 30009) and awards (kind 8) for display.
 *
 * The callback receives an array of GnostrProfileBadge with populated
 * definitions, or NULL if no badges are found.
 */
void gnostr_fetch_profile_badges_async(const gchar *pubkey_hex,
                                        GCancellable *cancellable,
                                        GnostrBadgeFetchCallback callback,
                                        gpointer user_data);

/**
 * GnostrBadgeDefinitionCallback:
 * @definition: The fetched badge definition, or NULL on error.
 * @user_data: User data passed to the fetch function.
 *
 * Callback for badge definition fetch operations.
 */
typedef void (*GnostrBadgeDefinitionCallback)(GnostrBadgeDefinition *definition,
                                               gpointer user_data);

/**
 * gnostr_fetch_badge_definition_async:
 * @naddr: NIP-33 address in format "30009:<pubkey>:<identifier>".
 * @cancellable: (nullable): Cancellable for the operation.
 * @callback: Callback when definition is fetched.
 * @user_data: User data for callback.
 *
 * Fetches a single badge definition by its addressable reference.
 */
void gnostr_fetch_badge_definition_async(const gchar *naddr,
                                          GCancellable *cancellable,
                                          GnostrBadgeDefinitionCallback callback,
                                          gpointer user_data);

/* ============== Badge Image Cache ============== */

/**
 * gnostr_badge_prefetch_image:
 * @url: Badge image URL.
 *
 * Prefetches a badge image into the cache for faster display.
 * Uses the existing avatar/image cache infrastructure.
 */
void gnostr_badge_prefetch_image(const gchar *url);

/**
 * gnostr_badge_get_cached_image:
 * @url: Badge image URL.
 *
 * Attempts to load a badge image from cache.
 *
 * Returns: (transfer full): Cached texture or NULL if not cached.
 */
GdkTexture *gnostr_badge_get_cached_image(const gchar *url);

G_END_DECLS

#endif /* NIP58_BADGES_H */
