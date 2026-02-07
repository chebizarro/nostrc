#include "nostr-enums.h"

GType
nostr_relay_state_get_type(void)
{
  static GType type = 0;

  if (g_once_init_enter(&type)) {
    static const GEnumValue values[] = {
      { NOSTR_RELAY_STATE_DISCONNECTED, "NOSTR_RELAY_STATE_DISCONNECTED", "disconnected" },
      { NOSTR_RELAY_STATE_CONNECTING, "NOSTR_RELAY_STATE_CONNECTING", "connecting" },
      { NOSTR_RELAY_STATE_CONNECTED, "NOSTR_RELAY_STATE_CONNECTED", "connected" },
      { NOSTR_RELAY_STATE_ERROR, "NOSTR_RELAY_STATE_ERROR", "error" },
      { 0, NULL, NULL }
    };
    GType t = g_enum_register_static("NostrRelayState", values);
    g_once_init_leave(&type, t);
  }

  return type;
}
