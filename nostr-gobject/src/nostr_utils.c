#include "nostr_utils.h"
#include "nostr_nip19.h"
#include <string.h>

/* Validate a 64-char hex string (only 0-9, a-f, A-F) */
static gboolean
is_hex64(const char *s)
{
  if (!s || strlen(s) != 64) return FALSE;
  for (int i = 0; i < 64; i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
      return FALSE;
  }
  return TRUE;
}

gchar *
gnostr_ensure_hex_pubkey(const char *input)
{
  if (!input || !*input) return NULL;

  /* Fast path: already 64-char hex */
  if (is_hex64(input))
    return g_strdup(input);

  /* Bech32 path: npub1... or nprofile1... */
  if (g_str_has_prefix(input, "npub1") || g_str_has_prefix(input, "nprofile1")) {
    GError *error = NULL;
    GNostrNip19 *nip19 = gnostr_nip19_decode(input, &error);
    if (!nip19) {
      g_warning("gnostr_ensure_hex_pubkey: failed to decode '%.*s...': %s",
                10, input, error ? error->message : "unknown");
      g_clear_error(&error);
      return NULL;
    }
    const gchar *hex = gnostr_nip19_get_pubkey(nip19);
    gchar *result = hex ? g_strdup(hex) : NULL;
    g_object_unref(nip19);
    return result;
  }

  /* Unknown format */
  g_warning("gnostr_ensure_hex_pubkey: unrecognized format '%.*s...' (len=%zu)",
            10, input, strlen(input));
  return NULL;
}
