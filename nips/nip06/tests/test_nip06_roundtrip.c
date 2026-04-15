#include "nip06.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* BIP32 vectors runner from test_nip06_vectors.c */
extern int run_bip32_vectors(void);

/* NIP-06 spec test vectors: verify known mnemonics produce expected keys */
static int test_nip06_spec_vectors(void) {
    struct {
        const char *mnemonic;
        const char *expected_privkey_hex;
        const char *expected_pubkey_hex;
    } vectors[] = {
        {
            "leader monkey parrot ring guide accident before fence cannon height naive bean",
            "7f7ff03d123792d6ac594bfa67bf6d0c0ab55b6b1fdb6249303fe861f1ccba9a",
            "17162c921dc4d2518f9a101db33695df1afb56ab82f5ff3e5da6eec3ca5cd917",
        },
        {
            "what bleak badge arrange retreat wolf trade produce cricket blur garlic valid proud rude strong choose busy staff weather area salt hollow arm fade",
            "c15d739894c81a2fcfd3a2df85a0d2c0dbc47a280d092799f144d73d7ae78add",
            "d41b22899549e1f3d335a31002cfd382174006e166d3e658e3a5eecdb6463573",
        },
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof(vectors)/sizeof(vectors[0]); ++i) {
        if (!nostr_nip06_validate_mnemonic(vectors[i].mnemonic)) {
            fprintf(stderr, "NIP-06 spec vector %zu: mnemonic validation failed\n", i);
            rc = 1;
            continue;
        }
        unsigned char *seed = nostr_nip06_seed_from_mnemonic(vectors[i].mnemonic);
        if (!seed) {
            fprintf(stderr, "NIP-06 spec vector %zu: seed derivation failed\n", i);
            rc = 1;
            continue;
        }
        char *sk_hex = nostr_nip06_private_key_from_seed(seed);
        free(seed);
        if (!sk_hex) {
            fprintf(stderr, "NIP-06 spec vector %zu: key derivation failed\n", i);
            rc = 1;
            continue;
        }
        if (strcmp(sk_hex, vectors[i].expected_privkey_hex) != 0) {
            fprintf(stderr, "NIP-06 spec vector %zu: private key mismatch\n  got:  %s\n  want: %s\n",
                    i, sk_hex, vectors[i].expected_privkey_hex);
            rc = 1;
        }
        free(sk_hex);
    }
    return rc;
}

static int do_roundtrip(int words) {
    // Generate mnemonic with requested word count by calling lower-level when needed
    char *mn = NULL;
    if (words == 24) {
        mn = nostr_nip06_generate_mnemonic();
    } else {
        // For non-24 counts, call internal generator directly for coverage
        extern char *nostr_bip39_generate(int word_count);
        mn = nostr_bip39_generate(words);
    }
    if (!mn) { fprintf(stderr, "gen failed for %d words\n", words); return 1; }
    if (!nostr_nip06_validate_mnemonic(mn)) {
        fprintf(stderr, "validate failed for %d words: %s\n", words, mn);
        free(mn);
        return 1;
    }
    nostr_secure_buf sb = nostr_nip06_seed_secure(mn);
    if (!sb.ptr || sb.len != 64) { fprintf(stderr, "seed(secure) failed for %d words\n", words); free(mn); return 1; }
    char *sk = nostr_nip06_private_key_from_seed((const unsigned char*)sb.ptr);
    secure_free(&sb);
    if (!sk) { fprintf(stderr, "derive failed for %d words\n", words); free(mn); return 1; }
    // Just sanity check hex length
    int rc = 0;
    if ((int)strlen(sk) != 64) { fprintf(stderr, "bad sk len for %d words\n", words); rc = 1; }
    free(sk);
    free(mn);
    return rc;
}

int main(void) {
    int rc = 0;
    rc |= do_roundtrip(12);
    rc |= do_roundtrip(15);
    rc |= do_roundtrip(18);
    rc |= do_roundtrip(21);
    rc |= do_roundtrip(24);
    /* Run BIP32 official test vectors */
    rc |= run_bip32_vectors();
    /* Run NIP-06 spec test vectors */
    rc |= test_nip06_spec_vectors();
    return rc;
}
