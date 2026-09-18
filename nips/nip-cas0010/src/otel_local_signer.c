/*
 * NIP-CAS-0010: TEST-ONLY local (raw private key) signer.
 *
 * SECURITY: this translation unit is NOT part of the shipped library. It is
 * compiled only when NOSTR_OTEL_ENABLE_TEST_SIGNER is defined, which the build
 * does for the module's own tests and (optionally) examples, or when the
 * default-OFF CMake option NOSTR_OTEL_ENABLE_TEST_SIGNER is turned on
 * explicitly. A default build of nostr_nip_cas0010_core contains no
 * nostr_otel_local_signer_* symbol at all, so nothing linking the library can
 * reach a raw-key signing path: production producers supply their own
 * NostrOtelSignFn backed by Signet / NIP-46.
 */

#ifdef NOSTR_OTEL_ENABLE_TEST_SIGNER

#include <stdlib.h>
#include <string.h>

#include "nostr-keys.h"
#include "otel_internal.h"

struct NostrOtelLocalSigner {
    char *privkey_hex;
    size_t privkey_len;
    char *pubkey_hex;
};

static int local_signer_sign(NostrEvent *event, void *user_data) {
    NostrOtelLocalSigner *signer = (NostrOtelLocalSigner *)user_data;
    if (!event || !signer || !signer->privkey_hex) return -1;
    return nostr_event_sign(event, signer->privkey_hex);
}

NostrOtelLocalSigner *nostr_otel_local_signer_new(const char *privkey_hex) {
    if (!privkey_hex || strlen(privkey_hex) != 64) return NULL;
    NostrOtelLocalSigner *signer = calloc(1, sizeof(*signer));
    if (!signer) return NULL;
    signer->privkey_hex = strdup(privkey_hex);
    if (!signer->privkey_hex) {
        free(signer);
        return NULL;
    }
    signer->privkey_len = strlen(signer->privkey_hex);
    signer->pubkey_hex = nostr_key_get_public(privkey_hex);
    if (!signer->pubkey_hex) {
        nostr_otel_local_signer_free(signer);
        return NULL;
    }
    return signer;
}

void nostr_otel_local_signer_free(NostrOtelLocalSigner *signer) {
    if (!signer) return;
    if (signer->privkey_hex) {
        /* Wipe the key copy before returning the allocation to the heap. */
        memset(signer->privkey_hex, 0, signer->privkey_len);
        free(signer->privkey_hex);
    }
    free(signer->pubkey_hex);
    free(signer);
}

int nostr_otel_local_signer_bind(NostrOtelLocalSigner *signer, NostrOtelSigner *out) {
    if (!signer || !out) return NOSTR_OTEL_ERR_INVALID_ARG;
    out->sign = local_signer_sign;
    out->user_data = signer;
    return NOSTR_OTEL_OK;
}

const char *nostr_otel_local_signer_pubkey(const NostrOtelLocalSigner *signer) {
    return signer ? signer->pubkey_hex : NULL;
}

#endif /* NOSTR_OTEL_ENABLE_TEST_SIGNER */
