/* note-card-data.h — Ref-counted data bucket for NoteCardRow content fields.
 *
 * All string/scalar data that describes an event card is stored in this struct.
 * The struct is ref-counted (gatomicrefcount) so it can be safely shared with
 * async callbacks that may outlive the current binding cycle.
 *
 * During re-bind, the old NoteCardData is unref'd and a new empty one is created.
 * Any in-flight callbacks holding refs to the old data can safely read from it
 * without corrupting the new event's state.
 *
 * SPDX-License-Identifier: MIT
 * nostrc-ncr-lifecycle: NoteCardRow lifecycle hardening Phase 5
 */

#ifndef NOTE_CARD_DATA_H
#define NOTE_CARD_DATA_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _NoteCardData NoteCardData;

struct _NoteCardData {
  gatomicrefcount ref_count;

  /* === Core identity === */
  gchar *id_hex;           /* Event hex ID (64 chars) */
  gchar *root_id;          /* Thread root event ID */
  gchar *parent_id;        /* Parent event ID (for replies) */
  gchar *pubkey_hex;       /* Author pubkey hex */
  gchar *parent_pubkey;    /* Parent event author pubkey */
  gint64 created_at;       /* Event timestamp */
  gint   event_kind;       /* NIP kind (1=text, 30023=article, etc.) */

  /* === Author info === */
  gchar *avatar_url;       /* Author avatar URL */
  gchar *nip05;            /* Author NIP-05 identifier */
  gchar *author_lud16;     /* Author lightning address */

  /* === Content === */
  gchar *content_text;     /* Raw text content (for clipboard) */

  /* === NIP-18 Repost === */
  gboolean is_repost;
  gchar *reposter_pubkey;
  gchar *reposter_display_name;
  gint64 repost_created_at;
  guint  repost_count;

  /* === NIP-57 Zap receipt === */
  gboolean is_zap_receipt;
  gchar *zap_sender_pubkey;
  gchar *zap_recipient_pubkey;
  gchar *zap_target_event_id;
  gint64 zap_amount_msat;

  /* === NIP-18 Quote === */
  gchar *quoted_event_id;

  /* === NIP-36 Sensitive content === */
  gboolean is_sensitive;
  gboolean sensitive_content_revealed;
  gchar *content_warning_reason;

  /* === NIP-23 Article === */
  gboolean is_article;
  gchar *article_d_tag;
  gchar *article_title;
  gchar *article_image_url;
  gint64 article_published_at;

  /* === NIP-71 Video === */
  gboolean is_video;
  gchar *video_d_tag;
  gchar *video_url;
  gchar *video_thumb_url;
  gchar *video_title;
  gint64 video_duration;
  gboolean video_is_vertical;
  gboolean video_player_shown;

  /* === NIP-48 Proxy === */
  gchar *proxy_id;
  gchar *proxy_protocol;

  /* === NIP-03 OTS === */
  gboolean has_ots_proof;
  gint   ots_status;
  gint64 ots_verified_timestamp;
  guint  ots_block_height;

  /* === Interaction state === */
  gboolean is_reply;
  gboolean is_thread_root;
  gboolean is_pinned;
  gboolean is_bookmarked;
  gboolean is_liked;
  guint    like_count;
  gint64   zap_total_msat;
  guint    zap_count;
  guint    reply_count;
  gboolean is_own_note;
  gboolean is_logged_in;
  guint    depth;
};

/**
 * note_card_data_new:
 *
 * Creates a new empty NoteCardData with ref count 1.
 *
 * Returns: (transfer full): A new #NoteCardData
 */
NoteCardData *note_card_data_new (void);

/**
 * note_card_data_ref:
 *
 * Atomically increments the reference count.
 *
 * Returns: (transfer full): @data with incremented ref count
 */
NoteCardData *note_card_data_ref (NoteCardData *data);

/**
 * note_card_data_unref:
 *
 * Atomically decrements the reference count. Frees all strings when it hits zero.
 */
void note_card_data_unref (NoteCardData *data);

/**
 * note_card_data_clear_strings:
 *
 * Frees all owned string fields and sets them to NULL.
 * Useful during re-bind to reset state without creating a new struct.
 */
void note_card_data_clear_strings (NoteCardData *data);

G_END_DECLS

#endif /* NOTE_CARD_DATA_H */
