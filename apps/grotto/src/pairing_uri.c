#include "pairing_uri.h"
#include <secure_buf.h>
#include <nostr/nip55l/signer_ops.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>

static gboolean invalid(GError **error) {
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid pairing data or unsupported relay URL");
  return FALSE;
}
gboolean grotto_relays_validate(const gchar *const *relays, gsize count, GError **error) {
  if (!relays || !count || count > 64) return invalid(error);
  for (gsize i = 0; i < count; i++) {
    if (!relays[i] || strlen(relays[i]) > 2048) return invalid(error);
    g_autoptr(GUri) uri = g_uri_parse(relays[i], G_URI_FLAGS_NONE, NULL);
    const gchar *host = uri ? g_uri_get_host(uri) : NULL;
    const gchar *scheme = uri ? g_uri_get_scheme(uri) : NULL;
    g_autofree gchar *lower = host ? g_ascii_strdown(host, -1) : NULL;
    gboolean loopback = lower && (!strcmp(lower, "127.0.0.1") || !strcmp(lower, "::1") || !strcmp(lower, "localhost"));
    if (!uri || !lower || !*lower || g_uri_get_userinfo(uri) || g_uri_get_fragment(uri) ||
        (!g_strcmp0(scheme, "ws") ? !loopback : g_strcmp0(scheme, "wss")) || g_str_has_suffix(lower, ".onion")) return invalid(error);
  }
  return TRUE;
}
static gboolean bounded(const gchar *s, gsize max) {
  if (!s) return TRUE;
  if (strlen(s) > max || !g_utf8_validate(s, -1, NULL)) return FALSE;
  for (const gchar *p = s; *p; p = g_utf8_next_char(p)) if (g_unichar_iscntrl(g_utf8_get_char(p))) return FALSE;
  return TRUE;
}
void grotto_connect_clear(NostrNip46ConnectURI *uri) {
  if (uri->secret) secure_wipe(uri->secret, strlen(uri->secret));
  nostr_nip46_uri_connect_free(uri);
}
gboolean grotto_connect_parse(const gchar *text, NostrNip46ConnectURI *out, GError **error) {
  memset(out, 0, sizeof(*out));
  if (!text || strlen(text) > 16384 || !g_utf8_validate(text, -1, NULL) ||
      !g_str_has_prefix(text, "nostrconnect://")) return invalid(error);
  if (nostr_nip46_uri_parse_connect(text, out)) { grotto_connect_clear(out); return invalid(error); }
  gboolean ok = out->client_pubkey_hex && strlen(out->client_pubkey_hex) == 64;
  for (guint i = 0; ok && i < 64; i++) ok = g_ascii_isxdigit(out->client_pubkey_hex[i]);
  ok = ok && out->secret && *out->secret && bounded(out->secret, 1024) && bounded(out->perms_csv, 2048) &&
      bounded(out->name, 160) && bounded(out->url, 2048) && bounded(out->image, 2048);
  if (!ok) { grotto_connect_clear(out); return invalid(error); }
  if (!grotto_relays_validate((const gchar *const *)out->relays, out->n_relays, error)) { grotto_connect_clear(out); return FALSE; }
  g_autofree gchar *canonical = NULL;
  if (nostr_nip55l_relays_from_list((const gchar *const *)out->relays, out->n_relays, &canonical)) { grotto_connect_clear(out); return invalid(error); }
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, canonical, -1, NULL)) { grotto_connect_clear(out); return invalid(error); }
  JsonArray *array = json_node_get_array(json_parser_get_root(parser));
  for (gsize i = 0; i < out->n_relays; i++) free(out->relays[i]);
  free(out->relays); out->n_relays = json_array_get_length(array);
  out->relays = g_new0(gchar *, out->n_relays + 1);
  for (gsize i = 0; i < out->n_relays; i++) out->relays[i] = g_strdup(json_array_get_string_element(array, i));
  for (guint i = 0; i < 64; i++) out->client_pubkey_hex[i] = g_ascii_tolower(out->client_pubkey_hex[i]);
  return TRUE;
}
GVariant *grotto_connect_preview(const NostrNip46ConnectURI *uri) {
  GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&b, "{sv}", "client_pubkey", g_variant_new_string(uri->client_pubkey_hex));
  g_variant_builder_add(&b, "{sv}", "claimed_name", g_variant_new_string(uri->name ? uri->name : ""));
  g_variant_builder_add(&b, "{sv}", "requested_permissions_hint", g_variant_new_string(uri->perms_csv ? uri->perms_csv : ""));
  g_variant_builder_add(&b, "{sv}", "relays", g_variant_new_strv((const gchar *const *)uri->relays, uri->n_relays));
  return g_variant_ref_sink(g_variant_builder_end(&b));
}
