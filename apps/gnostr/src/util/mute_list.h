/**
 * NIP-51 Mute List Service
 *
 * Provides mute list management for the gnostr GTK app.
 * Handles loading/parsing kind 10000 events and filtering content.
 */

#ifndef GNOSTR_MUTE_LIST_H
#define GNOSTR_MUTE_LIST_H

#include <gio/gio.h>
#include <stdbool.h>
#include <stdint.h>

G_BEGIN_DECLS

/* ---- Opaque Mute List Handle ---- */
typedef struct _GnostrMuteList GnostrMuteList;

/* ---- Initialization ---- */

/**
 * gnostr_mute_list_get_default:
 *
 * Gets the singleton mute list instance for the app.
 * Creates it on first call. Thread-safe.
 *
 * Returns: (transfer none): the shared mute list instance
 */
GnostrMuteList *gnostr_mute_list_get_default(void);

/**
 * gnostr_mute_list_shutdown:
 *
 * Releases the singleton instance. Call at app shutdown.
 */
void gnostr_mute_list_shutdown(void);

/* ---- Loading ---- */

/**
 * gnostr_mute_list_load_from_json:
 * @self: mute list instance
 * @event_json: JSON string of kind 10000 event
 *
 * Parses a kind 10000 mute list event and caches entries.
 * This is for loading from local storage or relay response.
 *
 * Returns: TRUE on success
 */
gboolean gnostr_mute_list_load_from_json(GnostrMuteList *self,
                                          const char *event_json);

/**
 * gnostr_mute_list_fetch_async:
 * @self: mute list instance
 * @pubkey_hex: user's public key (64 hex chars)
 * @relays: NULL-terminated array of relay URLs
 * @callback: callback when fetch completes
 * @user_data: user data for callback
 *
 * Fetches the user's mute list from relays asynchronously.
 */
typedef void (*GnostrMuteListFetchCallback)(GnostrMuteList *self,
                                             gboolean success,
                                             gpointer user_data);

void gnostr_mute_list_fetch_async(GnostrMuteList *self,
                                   const char *pubkey_hex,
                                   const char * const *relays,
                                   GnostrMuteListFetchCallback callback,
                                   gpointer user_data);

/* ---- Query Functions ---- */

/**
 * gnostr_mute_list_is_pubkey_muted:
 * @self: mute list instance
 * @pubkey_hex: public key to check (64 hex chars)
 *
 * Checks if a pubkey is on the mute list.
 *
 * Returns: TRUE if muted
 */
gboolean gnostr_mute_list_is_pubkey_muted(GnostrMuteList *self,
                                           const char *pubkey_hex);

/**
 * gnostr_mute_list_is_event_muted:
 * @self: mute list instance
 * @event_id_hex: event ID to check (64 hex chars)
 *
 * Checks if a specific event is on the mute list.
 *
 * Returns: TRUE if muted
 */
gboolean gnostr_mute_list_is_event_muted(GnostrMuteList *self,
                                          const char *event_id_hex);

/**
 * gnostr_mute_list_is_hashtag_muted:
 * @self: mute list instance
 * @hashtag: hashtag to check (without #)
 *
 * Checks if a hashtag is on the mute list.
 *
 * Returns: TRUE if muted
 */
gboolean gnostr_mute_list_is_hashtag_muted(GnostrMuteList *self,
                                            const char *hashtag);

/**
 * gnostr_mute_list_contains_muted_word:
 * @self: mute list instance
 * @content: text content to check
 *
 * Checks if content contains any muted words.
 *
 * Returns: TRUE if content contains muted word
 */
gboolean gnostr_mute_list_contains_muted_word(GnostrMuteList *self,
                                               const char *content);

/**
 * gnostr_mute_list_should_hide_event:
 * @self: mute list instance
 * @event_json: full event JSON
 *
 * Comprehensive check: tests author pubkey, event id, hashtags, and words.
 *
 * Returns: TRUE if event should be hidden
 */
gboolean gnostr_mute_list_should_hide_event(GnostrMuteList *self,
                                             const char *event_json);

/* ---- Modification Functions ---- */

/**
 * gnostr_mute_list_add_pubkey:
 * @self: mute list instance
 * @pubkey_hex: public key to mute (64 hex chars)
 * @is_private: TRUE to encrypt this entry
 *
 * Adds a pubkey to the mute list (locally).
 * Call gnostr_mute_list_save_async() to persist to relays.
 */
void gnostr_mute_list_add_pubkey(GnostrMuteList *self,
                                  const char *pubkey_hex,
                                  gboolean is_private);

/**
 * gnostr_mute_list_remove_pubkey:
 * @self: mute list instance
 * @pubkey_hex: public key to unmute (64 hex chars)
 *
 * Removes a pubkey from the mute list.
 */
void gnostr_mute_list_remove_pubkey(GnostrMuteList *self,
                                     const char *pubkey_hex);

/**
 * gnostr_mute_list_add_word:
 * @self: mute list instance
 * @word: word to mute
 * @is_private: TRUE to encrypt this entry
 *
 * Adds a word to the mute list.
 */
void gnostr_mute_list_add_word(GnostrMuteList *self,
                                const char *word,
                                gboolean is_private);

/**
 * gnostr_mute_list_remove_word:
 * @self: mute list instance
 * @word: word to unmute
 *
 * Removes a word from the mute list.
 */
void gnostr_mute_list_remove_word(GnostrMuteList *self,
                                   const char *word);

/**
 * gnostr_mute_list_add_hashtag:
 * @self: mute list instance
 * @hashtag: hashtag to mute (without #)
 * @is_private: TRUE to encrypt this entry
 *
 * Adds a hashtag to the mute list.
 */
void gnostr_mute_list_add_hashtag(GnostrMuteList *self,
                                   const char *hashtag,
                                   gboolean is_private);

/**
 * gnostr_mute_list_remove_hashtag:
 * @self: mute list instance
 * @hashtag: hashtag to unmute (without #)
 *
 * Removes a hashtag from the mute list.
 */
void gnostr_mute_list_remove_hashtag(GnostrMuteList *self,
                                      const char *hashtag);

/**
 * gnostr_mute_list_add_event:
 * @self: mute list instance
 * @event_id_hex: event ID to mute (64 hex chars)
 * @is_private: TRUE to encrypt this entry
 *
 * Adds an event to the mute list.
 */
void gnostr_mute_list_add_event(GnostrMuteList *self,
                                 const char *event_id_hex,
                                 gboolean is_private);

/**
 * gnostr_mute_list_remove_event:
 * @self: mute list instance
 * @event_id_hex: event ID to unmute (64 hex chars)
 *
 * Removes an event from the mute list.
 */
void gnostr_mute_list_remove_event(GnostrMuteList *self,
                                    const char *event_id_hex);

/* ---- Persistence ---- */

/**
 * gnostr_mute_list_save_async:
 * @self: mute list instance
 * @callback: callback when save completes
 * @user_data: user data for callback
 *
 * Signs and publishes the mute list to relays via signer IPC.
 */
typedef void (*GnostrMuteListSaveCallback)(GnostrMuteList *self,
                                            gboolean success,
                                            const char *error_msg,
                                            gpointer user_data);

void gnostr_mute_list_save_async(GnostrMuteList *self,
                                  GnostrMuteListSaveCallback callback,
                                  gpointer user_data);

/* ---- Accessors ---- */

/**
 * gnostr_mute_list_get_pubkeys:
 * @self: mute list instance
 * @count: (out): number of pubkeys
 *
 * Gets all muted pubkeys.
 *
 * Returns: (transfer container): array of pubkey hex strings (do not free strings)
 */
const char **gnostr_mute_list_get_pubkeys(GnostrMuteList *self, size_t *count);

/**
 * gnostr_mute_list_get_words:
 * @self: mute list instance
 * @count: (out): number of words
 *
 * Gets all muted words.
 *
 * Returns: (transfer container): array of word strings (do not free strings)
 */
const char **gnostr_mute_list_get_words(GnostrMuteList *self, size_t *count);

/**
 * gnostr_mute_list_get_hashtags:
 * @self: mute list instance
 * @count: (out): number of hashtags
 *
 * Gets all muted hashtags.
 *
 * Returns: (transfer container): array of hashtag strings (do not free strings)
 */
const char **gnostr_mute_list_get_hashtags(GnostrMuteList *self, size_t *count);

/**
 * gnostr_mute_list_get_events:
 * @self: mute list instance
 * @count: (out): number of events
 *
 * Gets all muted event IDs.
 *
 * Returns: (transfer container): array of event ID hex strings (do not free strings)
 */
const char **gnostr_mute_list_get_events(GnostrMuteList *self, size_t *count);

/**
 * gnostr_mute_list_is_dirty:
 * @self: mute list instance
 *
 * Returns: TRUE if there are unsaved changes
 */
gboolean gnostr_mute_list_is_dirty(GnostrMuteList *self);

/* ---- Signals ---- */

/**
 * "changed" signal
 *
 * Emitted when the mute list is modified locally or loaded from relays.
 * Connect to update UI filtering.
 */

G_END_DECLS

#endif /* GNOSTR_MUTE_LIST_H */
