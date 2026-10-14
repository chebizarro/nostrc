#ifndef GROTTO_PAIRING_URI_H
#define GROTTO_PAIRING_URI_H
#include <gio/gio.h>
#include <nostr/nip46/nip46_uri.h>
/* Pure validation: never dials, fetches metadata, or grants authority. The
 * caller owns the parsed secret and must clear it with grotto_connect_clear. */
gboolean grotto_relays_validate(const gchar *const *relays, gsize count, GError **error);
gboolean grotto_connect_parse(const gchar *uri, NostrNip46ConnectURI *out, GError **error);
void grotto_connect_clear(NostrNip46ConnectURI *uri);
GVariant *grotto_connect_preview(const NostrNip46ConnectURI *uri);
#endif
