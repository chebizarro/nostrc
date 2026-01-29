/**
 * gnostr-note-embed.h - NIP-21 embedded note widget
 *
 * Renders nostr: URI references as embedded note cards:
 * - nostr:note1... - event by ID
 * - nostr:nevent1... - event with relay hints
 * - nostr:npub1... - profile reference
 * - nostr:nprofile1... - profile with relay hints
 * - nostr:naddr1... - addressable event
 */

#ifndef GNOSTR_NOTE_EMBED_H
#define GNOSTR_NOTE_EMBED_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_NOTE_EMBED (gnostr_note_embed_get_type())

G_DECLARE_FINAL_TYPE(GnostrNoteEmbed, gnostr_note_embed, GNOSTR, NOTE_EMBED, GtkWidget)

/**
 * Signals:
 * - "clicked" (GnostrNoteEmbed *self, gpointer user_data)
 *   Emitted when the embed is clicked
 * - "profile-clicked" (GnostrNoteEmbed *self, gchar *pubkey_hex, gpointer user_data)
 *   Emitted when a profile link is clicked
 */

typedef struct _GnostrNoteEmbed GnostrNoteEmbed;

/**
 * gnostr_note_embed_new:
 *
 * Creates a new note embed widget.
 *
 * Returns: (transfer full): a new #GnostrNoteEmbed
 */
GnostrNoteEmbed *gnostr_note_embed_new(void);

/**
 * gnostr_note_embed_set_nostr_uri:
 * @self: a #GnostrNoteEmbed
 * @uri: the nostr: URI to display (e.g., "nostr:note1..." or "note1...")
 *
 * Sets the nostr URI to parse and display. Triggers async loading.
 */
void gnostr_note_embed_set_nostr_uri(GnostrNoteEmbed *self, const char *uri);

/**
 * gnostr_note_embed_set_event_id:
 * @self: a #GnostrNoteEmbed
 * @event_id_hex: 64-char hex event ID
 * @relay_hints: (nullable) (array zero-terminated=1): relay URLs to query
 *
 * Directly set an event ID to display.
 */
void gnostr_note_embed_set_event_id(GnostrNoteEmbed *self,
                                     const char *event_id_hex,
                                     const char * const *relay_hints);

/**
 * gnostr_note_embed_set_pubkey:
 * @self: a #GnostrNoteEmbed
 * @pubkey_hex: 64-char hex pubkey
 * @relay_hints: (nullable) (array zero-terminated=1): relay URLs to query
 *
 * Set a profile/pubkey to display as a profile embed.
 */
void gnostr_note_embed_set_pubkey(GnostrNoteEmbed *self,
                                   const char *pubkey_hex,
                                   const char * const *relay_hints);

/**
 * gnostr_note_embed_set_loading:
 * @self: a #GnostrNoteEmbed
 * @loading: whether to show loading state
 *
 * Manually set loading state.
 */
void gnostr_note_embed_set_loading(GnostrNoteEmbed *self, gboolean loading);

/**
 * gnostr_note_embed_set_error:
 * @self: a #GnostrNoteEmbed
 * @error_message: (nullable): error message to display
 *
 * Show error state with optional message.
 */
void gnostr_note_embed_set_error(GnostrNoteEmbed *self, const char *error_message);

/**
 * gnostr_note_embed_set_content:
 * @self: a #GnostrNoteEmbed
 * @author_display: display name of the author
 * @author_handle: @handle of the author
 * @content: the note content text
 * @timestamp: human-readable timestamp
 * @avatar_url: (nullable): URL for author avatar
 *
 * Manually populate the embed with content (called after fetch completes).
 */
void gnostr_note_embed_set_content(GnostrNoteEmbed *self,
                                    const char *author_display,
                                    const char *author_handle,
                                    const char *content,
                                    const char *timestamp,
                                    const char *avatar_url);

/**
 * gnostr_note_embed_set_profile:
 * @self: a #GnostrNoteEmbed
 * @display_name: profile display name
 * @handle: profile @handle/name
 * @about: (nullable): profile about text
 * @avatar_url: (nullable): profile avatar URL
 * @pubkey_hex: 64-char hex pubkey
 *
 * Populate embed with profile data (for npub/nprofile references).
 */
void gnostr_note_embed_set_profile(GnostrNoteEmbed *self,
                                    const char *display_name,
                                    const char *handle,
                                    const char *about,
                                    const char *avatar_url,
                                    const char *pubkey_hex);

/**
 * gnostr_note_embed_get_target_id:
 * @self: a #GnostrNoteEmbed
 *
 * Returns: (transfer none) (nullable): the event ID or pubkey hex being displayed
 */
const char *gnostr_note_embed_get_target_id(GnostrNoteEmbed *self);

/**
 * gnostr_note_embed_is_profile:
 * @self: a #GnostrNoteEmbed
 *
 * Returns: TRUE if this embed is a profile, FALSE if it's an event
 */
gboolean gnostr_note_embed_is_profile(GnostrNoteEmbed *self);

/**
 * gnostr_note_embed_set_cancellable:
 * @self: a #GnostrNoteEmbed
 * @cancellable: (nullable): External cancellable from parent widget
 *
 * Sets an external cancellable for all async operations. When the parent
 * widget is disposed, it cancels this cancellable, stopping all async
 * operations and preventing use-after-free in callbacks.
 */
void gnostr_note_embed_set_cancellable(GnostrNoteEmbed *self, GCancellable *cancellable);

/**
 * gnostr_note_embed_prepare_for_unbind:
 * @self: a #GnostrNoteEmbed
 *
 * Prepares the widget for unbinding from a list item. This cancels all async
 * operations and marks the widget as disposed to prevent callbacks from
 * accessing widget state during the unbind/dispose process.
 *
 * CRITICAL: Call this from the parent widget's prepare_for_unbind BEFORE
 * the parent starts its own cleanup. This prevents crashes where async
 * callbacks try to access widget memory during disposal.
 */
void gnostr_note_embed_prepare_for_unbind(GnostrNoteEmbed *self);

G_END_DECLS

#endif /* GNOSTR_NOTE_EMBED_H */
