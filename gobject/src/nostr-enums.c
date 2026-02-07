#include "nostr-enums.h"

GType
gnostr_relay_state_get_type(void)
{
  static GType type = 0;

  if (g_once_init_enter(&type)) {
    static const GEnumValue values[] = {
      { GNOSTR_RELAY_STATE_DISCONNECTED, "GNOSTR_RELAY_STATE_DISCONNECTED", "disconnected" },
      { GNOSTR_RELAY_STATE_CONNECTING, "GNOSTR_RELAY_STATE_CONNECTING", "connecting" },
      { GNOSTR_RELAY_STATE_CONNECTED, "GNOSTR_RELAY_STATE_CONNECTED", "connected" },
      { GNOSTR_RELAY_STATE_ERROR, "GNOSTR_RELAY_STATE_ERROR", "error" },
      { 0, NULL, NULL }
    };
    GType t = g_enum_register_static("GNostrRelayState", values);
    g_once_init_leave(&type, t);
  }

  return type;
}
