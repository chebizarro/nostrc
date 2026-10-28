/* Test-only initial personal-home snapshot for two running syncd writers. */
#include "nh_syncd.h"
#include "nostr-keys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: syncd_live_seed HOME STATE NSEC_HEX SEED_HEX\n");
        return 2;
    }
    uint8_t seed[32] = {0};
    char *pub = nostr_key_get_public(argv[3]);
    nh_syncd_state *state = NULL;
    int rc = !pub || nh_porthome_from_hex64(argv[4], seed) != 0 ||
             nh_syncd_state_new(argv[1], "nostr-homed.home.v1:personal",
                                pub, seed, &state) != NH_SYNCD_OK ||
             nh_syncd_state_save(state, argv[2]) != NH_SYNCD_OK;
    memset(seed, 0, sizeof seed);
    nh_syncd_state_free(state);
    free(pub);
    return rc ? 1 : 0;
}
