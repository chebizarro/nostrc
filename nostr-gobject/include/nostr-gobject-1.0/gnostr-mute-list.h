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
typedef struct _GNostrMuteList GNostrMuteList;

/* ---- Initialization ---- */

/**
 * gnostr_mute_list_get_default:
 *
 * Gets the singleton mute list instance for the app.
 * Creates it on first call. Thread-safe.
 *
 * Returns: (transfer none): the shared mute list instance
 */
GNostrMuteList *gnostr_mute_list_get_default(void);

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
gboolean gnostr_mute_list_load_from_json(GNostrMuteList *self,
                                          const char *event_json);

/**
 * GNostrMuteListMergeStrategy:
 * @GNOSTR_MUTE_LIST_MERGE_REMOTE_WINS: Remote data replaces local
 * @GNOSTR_MUTE_LIST_MERGE_LOCAL_WINS: Local data is kept (skip remote if local exists)
 * @GNOSTR_MUTE_LIST_MERGE_UNION: Merge lists (union of items)
 * @GNOSTR_MUTE_LIST_MERGE_LATEST: Keep data with newest timestamp
 */
typedef enum {
    GNOSTR_MUTE_LIST_MERGE_REMOTE_WINS,
    GNOSTR_MUTE_LIST_MERGE_LOCAL_WINS,
    GNOSTR_MUTE_LIST_MERGE_UNION,
    GNOSTR_MUTE_LIST_MERGE_LATEST
} GNostrMuteListMergeStrategy;

/**
 * gnostr_mute_list_fetch_async:
 * @self: mute list instance
 * @pubkey_hex: user's public key (64 hex chars)
 * @relays: NULL-terminated array of relay URLs
 * @callback: callback when fetch completes
 * @user_data: user data for callback
 *
 * Fetches the user's mute list from relays asynchronously.
 * Uses REMOTE_WINS strategy (replaces local with remote).
 */
typedef void (*GNostrMuteListFetchCallback)(GNostrMuteList *self,
                                             gboolean success,
                                             gpointer user_data);

void gnostr_mute_list_fetch_async(GNostrMuteList *self,
                                   const char *pubkey_hex,
                                   const char * const *relays,
                                   GNostrMuteListFetchCallback callback,
                                   gpointer user_data);

/**
 * gnostr_mute_list_fetch_with_strategy_async:
 * @self: mute list instance
 * @pubkey_hex: user's public key (64 hex chars)
 * @relays: NULL-terminated array of relay URLs
 * @strategy: merge strategy to use
 * @callback: callback when fetch completes
 * @user_data: user data for callback
 *
 * Fetches the user's mute list from relays asynchronously with
 * the specified merge strategy.
 */
void gnostr_mute_list_fetch_with_strategy_async(GNostrMuteList *self,
                                                 const char *pubkey_hex,
                                                 const char * const *relays,
                                                 GNostrMuteListMergeStrategy strategy,
                                                 GNostrMuteListFetchCallback callback,
                                                 gpointer user_data);

/**
 * gnostr_mute_list_get_last_event_time:
 * @self: mute list instance
 *
 * Gets the timestamp of the last loaded mute list event.
 *
 * Returns: timestamp or 0 if never loaded
 */
gint64 gnostr_mute_list_get_last_event_time(GNostrMuteList *self);

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
gboolean gnostr_mute_list_is_pubkey_muted(GNostrMuteList *self,
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
gboolean gnostr_mute_list_is_event_muted(GNostrMuteList *self,
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
gboolean gnostr_mute_list_is_hashtag_muted(GNostrMuteList *self,
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
gboolean gnostr_mute_list_contains_muted_word(GNostrMuteList *self,
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
gboolean gnostr_mute_list_should_hide_event(GNostrMuteList *self,
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
void gnostr_mute_list_add_pubkey(GNostrMuteList *self,
                                  const char *pubkey_hex,
                                  gboolean is_private);

/**
 * gnostr_mute_list_remove_pubkey:
 * @self: mute list instance
 * @pubkey_hex: public key to unmute (64 hex chars)
 *
 * Removes a pubkey from the mute list.
 */
void gnostr_mute_list_remove_pubkey(GNostrMuteList *self,
                                     const char *pubkey_hex);

/**
 * gnostr_mute_list_add_word:
 * @self: mute list instance
 * @word: word to mute
 * @is_private: TRUE to encrypt this entry
 *
 * Adds a word to the mute list.
 */
void gnostr_mute_list_add_word(GNostrMuteList *self,
                                const char *word,
                                gboolean is_private);

/**
 * gnostr_mute_list_remove_word:
 * @self: mute list instance
 * @word: word to unmute
 *
 * Removes a word from the mute list.
 */
void gnostr_mute_list_remove_word(GNostrMuteList *self,
                                   const char *word);

/**
 * gnostr_mute_list_add_hashtag:
 * @self: mute list instance
 * @hashtag: hashtag to mute (without #)
 * @is_private: TRUE to encrypt this entry
 *
 * Adds a hashtag to the mute list.
 */
void gnostr_mute_list_add_hashtag(GNostrMuteList *self,
                                   const char *hashtag,
                                   gboolean is_private);

/**
 * gnostr_mute_list_remove_hashtag:
 * @self: mute list instance
 * @hashtag: hashtag to unmute (without #)
 *
 * Removes a hashtag from the mute list.
 */
void gnostr_mute_list_remove_hashtag(GNostrMuteList *self,
                                      const char *hashtag);

/**
 * gnostr_mute_list_add_event:
 * @self: mute list instance
 * @event_id_hex: event ID to mute (64 hex chars)
 * @is_private: TRUE to encrypt this entry
 *
 * Adds an event to the mute list.
 */
void gnostr_mute_list_add_event(GNostrMuteList *self,
                                 const char *event_id_hex,
                                 gboolean is_private);

/**
 * gnostr_mute_list_remove_event:
 * @self: mute list instance
 * @event_id_hex: event ID to unmute (64 hex chars)
 *
 * Removes an event from the mute list.
 */
void gnostr_mute_list_remove_event(GNostrMuteList *self,
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
typedef void (*GNostrMuteListSaveCallback)(GNostrMuteList *self,
                                            gboolean success,
                                            const char *error_msg,
                                            gpointer user_data);

void gnostr_mute_list_save_async(GNostrMuteList *self,
                                  GNostrMuteListSaveCallback callback,
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
const char **gnostr_mute_list_get_pubkeys(GNostrMuteList *self, size_t *count);

/**
 * gnostr_mute_list_get_words:
 * @self: mute list instance
 * @count: (out): number of words
 *
 * Gets all muted words.
 *
 * Returns: (transfer container): array of word strings (do not free strings)
 */
const char **gnostr_mute_list_get_words(GNostrMuteList *self, size_t *count);

/**
 * gnostr_mute_list_get_hashtags:
 * @self: mute list instance
 * @count: (out): number of hashtags
 *
 * Gets all muted hashtags.
 *
 * Returns: (transfer container): array of hashtag strings (do not free strings)
 */
const char **gnostr_mute_list_get_hashtags(GNostrMuteList *self, size_t *count);

/**
 * gnostr_mute_list_get_events:
 * @self: mute list instance
 * @count: (out): number of events
 *
 * Gets all muted event IDs.
 *
 * Returns: (transfer container): array of event ID hex strings (do not free strings)
 */
const char **gnostr_mute_list_get_events(GNostrMuteList *self, size_t *count);

/**
 * gnostr_mute_list_is_dirty:
 * @self: mute list instance
 *
 * Returns: TRUE if there are unsaved changes
 */
gboolean gnostr_mute_list_is_dirty(GNostrMuteList *self);

/* ---- Signals ---- */

/**
 * "changed" signal
 *
 * Emitted when the mute list is modified locally or loaded from relays.
 * Connect to update UI filtering.
 */

G_END_DECLS

#endif /* GNOSTR_MUTE_LIST_H */
