#ifndef NOSTR_NIP19_H
#define NOSTR_NIP19_H

#include "nostr-event.h"
#include <stdbool.h>
#include <stdint.h>

// TLV entry types
#define TLV_DEFAULT 0
#define TLV_RELAY 1
#define TLV_AUTHOR 2
#define TLV_KIND 3

// Decode a Bech32 encoded string
int nip19_decode(const char *bech32_string, char *prefix, size_t prefix_len, void **value, size_t *value_len);

// Encode a private key as Bech32
char *nip19_encode_private_key(const char *private_key_hex);

// Encode a public key as Bech32
char *nip19_encode_public_key(const char *public_key_hex);

// Encode an event ID as Bech32
char *nip19_encode_note_id(const char *event_id_hex);

// Encode a profile as Bech32
char *nip19_encode_profile(const char *public_key_hex, const char *relays[], size_t relays_len);

// Encode an event as Bech32
char *nip19_encode_event(const char *event_id_hex, const char *relays[], size_t relays_len, const char *author_hex);

// Encode an entity as Bech32
char *nip19_encode_entity(const char *public_key_hex, int kind, const char *identifier, const char *relays[], size_t relays_len);

#endif // NOSTR_NIP19_H
