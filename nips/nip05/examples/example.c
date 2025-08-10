#include "nip05.h"
#include <stdio.h>
#include <stdlib.h>

int main() {
    const char *identifier = "chebizarro@coinos.io"; // replace with a real domain for live test

    char *pub = NULL; char **relays = NULL; size_t nrel = 0; char *err = NULL;
    int rc = nostr_nip05_lookup(identifier, &pub, &relays, &nrel, &err);
    if (rc != 0) {
        fprintf(stderr, "lookup failed: %s\n", err ? err : "unknown");
        free(err);
        return 1;
    }
    printf("pubkey: %s\n", pub);
    for (size_t i = 0; i < nrel; i++) if (relays && relays[i]) printf("relay[%zu]: %s\n", i, relays[i]);
    if (relays) { for (size_t i = 0; i < nrel; i++) free(relays[i]); free(relays); }
    free(pub);
    return 0;
}
