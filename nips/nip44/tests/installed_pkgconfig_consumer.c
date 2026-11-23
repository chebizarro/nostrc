/* Consume the installed module, including its real core/crypto closure. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nostr-keys.h>
#include <nostr/nip44/nip44.h>
#include <nostr/nip44/kdf.h>

int main(void) {
    uint8_t secret[32] = {0}, peer[32], conversation[32];
    secret[31] = 1;
    char *public_hex = nostr_key_get_public("0000000000000000000000000000000000000000000000000000000000000001");
    assert(public_hex && strlen(public_hex) == 64);
    for (size_t i = 0; i < sizeof(peer); ++i) {
        unsigned value;
        assert(sscanf(public_hex + i * 2, "%2x", &value) == 1);
        peer[i] = (uint8_t)value;
    }
    free(public_hex);
    assert(nostr_nip44_convkey(secret, peer, conversation) == 0);
    uint8_t expanded[32];
    assert(nostr_nip44_hkdf_expand(conversation, NULL, 0, expanded, sizeof(expanded)) == 0);
    const uint8_t message[] = "installed static NIP-44 consumer";
    char *payload = NULL;
    assert(nostr_nip44_encrypt_v2(secret, peer, message, sizeof(message) - 1, &payload) == 0);
    assert(payload);
    uint8_t *plain = NULL;
    size_t length = 0;
    assert(nostr_nip44_decrypt_v2(secret, peer, payload, &plain, &length) == 0);
    assert(length == sizeof(message) - 1 && memcmp(plain, message, length) == 0);
    free(plain);
    free(payload);
    return 0;
}
