#ifndef GH_TEST_ACCOUNT_STORE_OUTBOX_H
#define GH_TEST_ACCOUNT_STORE_OUTBOX_H

/* GhOutbox calls for test_account_store.c, in their own translation unit:
 * gh-outbox.h's GhMessageStatus cannot meet the conversation model's (see
 * gh-app-outbox.h). */
#include <glib-object.h>

/* T-enqueue a text from the store's account; returns its outbox id. */
gint64 test_outbox_send(GObject *outbox, const gchar *recipient_hex, const gchar *text);
/* Whether the outbox holds (or loads from its store) that message. */
gboolean test_outbox_has(GObject *outbox, gint64 outbox_id);

#endif
