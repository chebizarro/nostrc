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

GType
gnostr_nip46_state_get_type(void)
{
  static GType type = 0;

  if (g_once_init_enter(&type)) {
    static const GEnumValue values[] = {
      { GNOSTR_NIP46_STATE_DISCONNECTED, "GNOSTR_NIP46_STATE_DISCONNECTED", "disconnected" },
      { GNOSTR_NIP46_STATE_CONNECTING, "GNOSTR_NIP46_STATE_CONNECTING", "connecting" },
      { GNOSTR_NIP46_STATE_CONNECTED, "GNOSTR_NIP46_STATE_CONNECTED", "connected" },
      { GNOSTR_NIP46_STATE_STOPPING, "GNOSTR_NIP46_STATE_STOPPING", "stopping" },
      { 0, NULL, NULL }
    };
    GType t = g_enum_register_static("GNostrNip46State", values);
    g_once_init_leave(&type, t);
  }

  return type;
}

GType
gnostr_subscription_state_get_type(void)
{
  static GType type = 0;

  if (g_once_init_enter(&type)) {
    static const GEnumValue values[] = {
      { GNOSTR_SUBSCRIPTION_STATE_PENDING, "GNOSTR_SUBSCRIPTION_STATE_PENDING", "pending" },
      { GNOSTR_SUBSCRIPTION_STATE_ACTIVE, "GNOSTR_SUBSCRIPTION_STATE_ACTIVE", "active" },
      { GNOSTR_SUBSCRIPTION_STATE_EOSE_RECEIVED, "GNOSTR_SUBSCRIPTION_STATE_EOSE_RECEIVED", "eose-received" },
      { GNOSTR_SUBSCRIPTION_STATE_CLOSED, "GNOSTR_SUBSCRIPTION_STATE_CLOSED", "closed" },
      { GNOSTR_SUBSCRIPTION_STATE_ERROR, "GNOSTR_SUBSCRIPTION_STATE_ERROR", "error" },
      { 0, NULL, NULL }
    };
    GType t = g_enum_register_static("GNostrSubscriptionState", values);
    g_once_init_leave(&type, t);
  }

  return type;
}
