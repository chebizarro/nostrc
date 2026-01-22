#ifndef GNOSTR_NOTE_CARD_ROW_H
#define GNOSTR_NOTE_CARD_ROW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_NOTE_CARD_ROW (gnostr_note_card_row_get_type())

G_DECLARE_FINAL_TYPE(GnostrNoteCardRow, gnostr_note_card_row, GNOSTR, NOTE_CARD_ROW, GtkWidget)

/* Signals
 * "open-nostr-target" (gchar* target, gpointer user_data)
 * "open-url" (gchar* url, gpointer user_data)
 * "request-embed" (gchar* target, gpointer user_data)
 * "open-profile" (gchar* pubkey_hex, gpointer user_data)
 * "reply-requested" (gchar* id_hex, gchar* root_id, gchar* pubkey_hex, gpointer user_data)
 * "repost-requested" (gchar* id_hex, gchar* pubkey_hex, gpointer user_data)
 * "quote-requested" (gchar* id_hex, gchar* pubkey_hex, gpointer user_data)
 * "like-requested" (gchar* id_hex, gchar* pubkey_hex, gpointer user_data)
 */

typedef struct _GnostrNoteCardRow GnostrNoteCardRow;

GnostrNoteCardRow *gnostr_note_card_row_new(void);

void gnostr_note_card_row_set_author(GnostrNoteCardRow *self, const char *display_name, const char *handle, const char *avatar_url);
void gnostr_note_card_row_set_timestamp(GnostrNoteCardRow *self, gint64 created_at, const char *fallback_ts);
void gnostr_note_card_row_set_content(GnostrNoteCardRow *self, const char *content);
void gnostr_note_card_row_set_depth(GnostrNoteCardRow *self, guint depth);
void gnostr_note_card_row_set_ids(GnostrNoteCardRow *self, const char *id_hex, const char *root_id, const char *pubkey_hex);
void gnostr_note_card_row_set_embed(GnostrNoteCardRow *self, const char *title, const char *snippet);
/* Rich embed variant: title (e.g., Note), meta (e.g., author · time), snippet (content excerpt) */
void gnostr_note_card_row_set_embed_rich(GnostrNoteCardRow *self, const char *title, const char *meta, const char *snippet);

/* NIP-05 verification: set identifier and trigger async verification */
void gnostr_note_card_row_set_nip05(GnostrNoteCardRow *self, const char *nip05, const char *pubkey_hex);

G_END_DECLS

#endif /* GNOSTR_NOTE_CARD_ROW_H */
