#include "account-store-outbox.h"

#include "gh-outbox.h"

gint64
test_outbox_send(GObject *outbox, const gchar *recipient_hex, const gchar *text)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GhOutboxItem) item = gh_outbox_send(GH_OUTBOX(outbox), recipient_hex, text, &error);
  g_assert_no_error(error);
  g_assert_nonnull(item);
  return gh_outbox_item_get_outbox_id(item);
}

gboolean
test_outbox_has(GObject *outbox, gint64 outbox_id)
{
  g_autoptr(GhOutboxItem) item = gh_outbox_lookup(GH_OUTBOX(outbox), outbox_id);
  return item != NULL;
}
