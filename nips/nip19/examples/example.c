#include <stdio.h>
#include "nostr/nip19.h"

int main() {
    // Example usage of NIP-19 functions
    const char *private_key_hex = "your-private-key-hex";
    char *nsec = nip19_encode_private_key(private_key_hex);
    if (nsec) {
        printf("Bech32 Encoded Private Key: %s\n", nsec);
        free(nsec);
    } else {
        printf("Failed to encode private key\n");
    }

    const char *public_key_hex = "your-public-key-hex";
    char *npub = nip19_encode_public_key(public_key_hex);
    if (npub) {
        printf("Bech32 Encoded Public Key: %s\n", npub);
        free(npub);
    } else {
        printf("Failed to encode public key\n");
    }

    const char *event_id_hex = "your-event-id-hex";
    char *note = nip19_encode_note_id(event_id_hex);
    if (note) {
        printf("Bech32 Encoded Event ID: %s\n", note);
        free(note);
    } else {
        printf("Failed to encode event ID\n");
    }

    return 0;
}
