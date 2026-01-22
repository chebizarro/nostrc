#ifndef GNOSTR_COMPOSER_H
#define GNOSTR_COMPOSER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_COMPOSER (gnostr_composer_get_type())

G_DECLARE_FINAL_TYPE(GnostrComposer, gnostr_composer, GNOSTR, COMPOSER, GtkWidget)

GtkWidget *gnostr_composer_new(void);

/* Clear the composer text */
void gnostr_composer_clear(GnostrComposer *self);

/* Reply context for NIP-10 threading */
void gnostr_composer_set_reply_context(GnostrComposer *self,
                                       const char *reply_to_id,
                                       const char *root_id,
                                       const char *reply_to_pubkey,
                                       const char *reply_to_display_name);
void gnostr_composer_clear_reply_context(GnostrComposer *self);
gboolean gnostr_composer_is_reply(GnostrComposer *self);
const char *gnostr_composer_get_reply_to_id(GnostrComposer *self);
const char *gnostr_composer_get_root_id(GnostrComposer *self);
const char *gnostr_composer_get_reply_to_pubkey(GnostrComposer *self);

/* Quote context for NIP-18 quote posts (kind 1 with q-tag) */
void gnostr_composer_set_quote_context(GnostrComposer *self,
                                       const char *quote_id,
                                       const char *quote_pubkey,
                                       const char *nostr_uri,
                                       const char *quoted_author_display_name);
void gnostr_composer_clear_quote_context(GnostrComposer *self);
gboolean gnostr_composer_is_quote(GnostrComposer *self);
const char *gnostr_composer_get_quote_id(GnostrComposer *self);
const char *gnostr_composer_get_quote_pubkey(GnostrComposer *self);
const char *gnostr_composer_get_quote_nostr_uri(GnostrComposer *self);

/* Media upload state (Blossom) */
gboolean gnostr_composer_is_uploading(GnostrComposer *self);
void gnostr_composer_cancel_upload(GnostrComposer *self);

G_END_DECLS

#endif /* GNOSTR_COMPOSER_H */
