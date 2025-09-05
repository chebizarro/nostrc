#include "nip06.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* BIP32 vectors runner from test_nip06_vectors.c */
extern int run_bip32_vectors(void);

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
    return rc;
}
