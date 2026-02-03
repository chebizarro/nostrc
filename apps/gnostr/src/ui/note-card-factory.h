/* note-card-factory.h
 *
 * Unified factory for NoteCardRow creation and lifecycle management.
 * nostrc-o7pp: Ensures consistent bind/unbind handling across all views.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef NOTE_CARD_FACTORY_H
#define NOTE_CARD_FACTORY_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Forward declaration - full type in note_card_row.h */
typedef struct _GnostrNoteCardRow GnostrNoteCardRow;

#define NOTE_CARD_TYPE_FACTORY (note_card_factory_get_type())

G_DECLARE_FINAL_TYPE(NoteCardFactory, note_card_factory, NOTE_CARD, FACTORY, GObject)

/**
 * NoteCardBindFlags:
 * @NOTE_CARD_BIND_BASIC: Basic binding (author, timestamp, content, ids)
 * @NOTE_CARD_BIND_THREAD_INFO: Include thread info (parent, reply indicator)
 * @NOTE_CARD_BIND_REACTIONS: Include reaction stats (likes, zaps)
 * @NOTE_CARD_BIND_REPOSTS: Handle repost display (kind 6)
 * @NOTE_CARD_BIND_ARTICLES: Handle long-form content (kind 30023)
 * @NOTE_CARD_BIND_VIDEOS: Handle video events (kind 34235/34236)
 * @NOTE_CARD_BIND_HASHTAGS: Extract and display hashtags
 * @NOTE_CARD_BIND_CONTENT_WARNING: Handle NIP-36 sensitive content
 * @NOTE_CARD_BIND_ALL: All features enabled
 *
 * Flags to control what features are enabled during row binding.
 * Different views may want different subsets of functionality.
 */
typedef enum {
  NOTE_CARD_BIND_BASIC           = 0,
  NOTE_CARD_BIND_THREAD_INFO     = 1 << 0,
  NOTE_CARD_BIND_REACTIONS       = 1 << 1,
  NOTE_CARD_BIND_REPOSTS         = 1 << 2,
  NOTE_CARD_BIND_ARTICLES        = 1 << 3,
  NOTE_CARD_BIND_VIDEOS          = 1 << 4,
  NOTE_CARD_BIND_HASHTAGS        = 1 << 5,
  NOTE_CARD_BIND_CONTENT_WARNING = 1 << 6,
  NOTE_CARD_BIND_ALL             = 0x7F
} NoteCardBindFlags;

/**
 * NoteCardFactorySignalFlags:
 * @NOTE_CARD_SIGNAL_OPEN_PROFILE: Connect open-profile signal
 * @NOTE_CARD_SIGNAL_VIEW_THREAD: Connect view-thread-requested signal
 * @NOTE_CARD_SIGNAL_REPLY: Connect reply-requested signal
 * @NOTE_CARD_SIGNAL_REPOST: Connect repost-requested signal
 * @NOTE_CARD_SIGNAL_QUOTE: Connect quote-requested signal
 * @NOTE_CARD_SIGNAL_LIKE: Connect like-requested signal
 * @NOTE_CARD_SIGNAL_ZAP: Connect zap-requested signal
 * @NOTE_CARD_SIGNAL_MUTE: Connect mute signals
 * @NOTE_CARD_SIGNAL_BOOKMARK: Connect bookmark-toggled signal
 * @NOTE_CARD_SIGNAL_DELETE: Connect delete-note-requested signal
 * @NOTE_CARD_SIGNAL_ALL: All signals connected
 *
 * Flags to control which signals are connected during row setup.
 */
typedef enum {
  NOTE_CARD_SIGNAL_NONE         = 0,
  NOTE_CARD_SIGNAL_OPEN_PROFILE = 1 << 0,
  NOTE_CARD_SIGNAL_VIEW_THREAD  = 1 << 1,
  NOTE_CARD_SIGNAL_REPLY        = 1 << 2,
  NOTE_CARD_SIGNAL_REPOST       = 1 << 3,
  NOTE_CARD_SIGNAL_QUOTE        = 1 << 4,
  NOTE_CARD_SIGNAL_LIKE         = 1 << 5,
  NOTE_CARD_SIGNAL_ZAP          = 1 << 6,
  NOTE_CARD_SIGNAL_MUTE         = 1 << 7,
  NOTE_CARD_SIGNAL_BOOKMARK     = 1 << 8,
  NOTE_CARD_SIGNAL_DELETE       = 1 << 9,
  NOTE_CARD_SIGNAL_NAVIGATE     = 1 << 10,
  NOTE_CARD_SIGNAL_HASHTAG      = 1 << 11,
  NOTE_CARD_SIGNAL_ALL          = 0xFFF
} NoteCardFactorySignalFlags;

/**
 * note_card_factory_new:
 * @bind_flags: Features to enable during binding
 * @signal_flags: Signals to connect during setup
 *
 * Creates a new NoteCardFactory with the specified configuration.
 *
 * Returns: (transfer full): A new #NoteCardFactory
 */
NoteCardFactory *note_card_factory_new(NoteCardBindFlags bind_flags,
                                       NoteCardFactorySignalFlags signal_flags);

/**
 * note_card_factory_get_gtk_factory:
 * @self: the factory
 *
 * Get the underlying GtkListItemFactory for use with GtkListView.
 *
 * Returns: (transfer none): The #GtkListItemFactory
 */
GtkListItemFactory *note_card_factory_get_gtk_factory(NoteCardFactory *self);

/**
 * note_card_factory_set_user_data:
 * @self: the factory
 * @user_data: user data to pass to signal handlers
 *
 * Set user data that will be passed to connected signal handlers.
 * Typically this is the parent view widget.
 */
void note_card_factory_set_user_data(NoteCardFactory *self, gpointer user_data);

/**
 * note_card_factory_connect_open_profile:
 * @self: the factory
 * @callback: callback function
 * @user_data: user data for callback
 *
 * Connect a custom handler for the open-profile signal.
 * The callback signature is: void (*)(const char *pubkey_hex, gpointer user_data)
 */
void note_card_factory_connect_open_profile(NoteCardFactory *self,
                                            GCallback callback,
                                            gpointer user_data);

/**
 * note_card_factory_connect_view_thread:
 * @self: the factory
 * @callback: callback function
 * @user_data: user data for callback
 *
 * Connect a custom handler for the view-thread-requested signal.
 * The callback signature is: void (*)(const char *root_event_id, gpointer user_data)
 */
void note_card_factory_connect_view_thread(NoteCardFactory *self,
                                           GCallback callback,
                                           gpointer user_data);

/**
 * note_card_factory_connect_reply:
 * @self: the factory
 * @callback: callback function
 * @user_data: user data for callback
 *
 * Connect a custom handler for the reply-requested signal.
 */
void note_card_factory_connect_reply(NoteCardFactory *self,
                                     GCallback callback,
                                     gpointer user_data);

/**
 * note_card_factory_connect_search_hashtag:
 * @self: the factory
 * @callback: callback function
 * @user_data: user data for callback
 *
 * Connect a custom handler for the search-hashtag signal.
 */
void note_card_factory_connect_search_hashtag(NoteCardFactory *self,
                                              GCallback callback,
                                              gpointer user_data);

/**
 * NoteCardBindCallback:
 * @row: the NoteCardRow widget to populate
 * @item: the model item (GObject) being bound
 * @user_data: user data from note_card_factory_set_bind_callback
 *
 * Custom callback for populating NoteCardRow from model item.
 * Called after prepare_for_bind but before visibility is set.
 *
 * Views with custom data models can use this to handle their own
 * data binding logic instead of relying on the default binding.
 */
typedef void (*NoteCardBindCallback)(GnostrNoteCardRow *row,
                                     GObject *item,
                                     gpointer user_data);

/**
 * note_card_factory_set_bind_callback:
 * @self: the factory
 * @callback: custom bind callback, or NULL to use default binding
 * @user_data: user data for callback
 *
 * Set a custom bind callback for populating rows from model items.
 * If set, the factory will call this instead of the default binding logic.
 * The callback is called after prepare_for_bind.
 */
void note_card_factory_set_bind_callback(NoteCardFactory *self,
                                         NoteCardBindCallback callback,
                                         gpointer user_data);

G_END_DECLS

#endif /* NOTE_CARD_FACTORY_H */
