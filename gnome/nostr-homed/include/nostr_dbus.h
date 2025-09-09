#ifndef NOSTR_DBUS_H
#define NOSTR_DBUS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Probes for available signer bus names and returns the one to use. */
const char *nh_signer_bus_name(void);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_DBUS_H */
