#include "nip06.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const char *mnemonic = "leader monkey parrot ring guide accident before fence cannon height naive bean";
    const char *want_sk = "7f7ff03d123792d6ac594bfa67bf6d0c0ab55b6b1fdb6249303fe861f1ccba9a";

    if (!nostr_nip06_validate_mnemonic(mnemonic)) {
        fprintf(stderr, "mnemonic failed validation\n");
        return 1;
    }
    unsigned char *seed = nostr_nip06_seed_from_mnemonic(mnemonic);
    if (!seed) { fprintf(stderr, "seed_from_mnemonic failed\n"); return 1; }
    char *sk = nostr_nip06_private_key_from_seed(seed);
    free(seed);
    if (!sk) { fprintf(stderr, "private_key_from_seed failed\n"); return 1; }

    int rc = 0;
    if (strcmp(sk, want_sk) != 0) {
        fprintf(stderr, "sk mismatch\n got:  %s\n want: %s\n", sk, want_sk);
        rc = 1;
    }
    free(sk);
    return rc;
}
