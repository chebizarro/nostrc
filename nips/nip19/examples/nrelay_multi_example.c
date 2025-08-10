#include "nip19.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    const char *relays[] = {
        "wss://r.x.com",
        "wss://relay.example.com",
        "wss://nostr.example.org"
    };
    char *bech = NULL;
    if (nostr_nip19_encode_nrelay_multi(relays, 3, &bech) != 0) {
        fprintf(stderr, "encode failed\n");
        return 1;
    }
    printf("nrelay (3 relays): %s\n", bech);

    char **out_relays = NULL; size_t cnt = 0;
    if (nostr_nip19_decode_nrelay(bech, &out_relays, &cnt) != 0) {
        fprintf(stderr, "decode failed\n");
        free(bech);
        return 1;
    }
    printf("decoded relays (%zu):\n", cnt);
    for (size_t i = 0; i < cnt; ++i) {
        printf("  - %s\n", out_relays[i]);
        free(out_relays[i]);
    }
    free(out_relays);
    free(bech);
    return 0;
}
