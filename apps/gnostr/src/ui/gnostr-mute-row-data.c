#define G_LOG_DOMAIN "gnostr-mute-row-data"

#include "gnostr-mute-row-data.h"

#include <nostr-gobject-1.0/nostr_nip19.h>
#include <string.h>

GnostrMuteRowBinding *
gnostr_mute_row_binding_new(const char        *canonical_value,
                            GnostrMuteRowType  type)
{
  if (!canonical_value || !*canonical_value)
    return NULL;

  GnostrMuteRowBinding *binding = g_new0(GnostrMuteRowBinding, 1);
  binding->canonical_value = g_strdup(canonical_value);
  binding->type = type;

  switch (type) {
    case GNOSTR_MUTE_ROW_USER:
      if (strlen(canonical_value) == 64) {
        g_autoptr(GNostrNip19) n19 = gnostr_nip19_encode_npub(canonical_value, NULL);
        if (n19)
          binding->display_value = g_strdup(gnostr_nip19_get_bech32(n19));
      }
      if (!binding->display_value)
        binding->display_value = g_strdup(canonical_value);
      break;
    case GNOSTR_MUTE_ROW_HASHTAG:
      binding->display_value = g_strdup_printf("#%s", canonical_value);
      break;
    case GNOSTR_MUTE_ROW_WORD:
    default:
      binding->display_value = g_strdup(canonical_value);
      break;
  }

  return binding;
}

void
gnostr_mute_row_binding_free(GnostrMuteRowBinding *binding)
{
  if (!binding)
    return;

  g_free(binding->display_value);
  g_free(binding->canonical_value);
  g_free(binding);
}
