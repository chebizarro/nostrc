#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nip46.h"
#include "nip04.h"
#include "nostr.h"

// Sample public and private keys for demonstration purposes
const char *pubkey = "03a34b3d9e3c5e4b1eebba47c33b39bc14d2a947bb1f27c7b84d65fdd3f6b7a6ac";
const char *privkey = "5J3mBbAH58CERBBxgHiTr2Y29RbJ5jA63ZdG9yKL9jSJGhzwuoh";

void auth_callback(char *auth_url) {
    printf("Authentication URL: %s\n", auth_url);
}

int main() {
    // Create a BunkerClient
    char *bunker_url = "bunker://03a34b3d9e3c5e4b1eebba47c33b39bc14d2a947bb1f27c7b84d65fdd3f6b7a6ac?secret=my_secret&relay=wss://relay.nostr.example.com";
    nostr_simple_pool_t *pool = nostr_simple_pool_new();
    
    nip46_bunker_client_t *bunker_client = nip46_connect_bunker(privkey, bunker_url, pool, auth_callback);
    if (bunker_client == NULL) {
        fprintf(stderr, "Failed to connect to bunker\n");
        return 1;
    }

    // Ping the BunkerClient
    if (nip46_rpc(bunker_client, "ping", NULL, 0) != NULL) {
        printf("Ping successful\n");
    } else {
        fprintf(stderr, "Ping failed\n");
    }

    // Get the public key from the BunkerClient
    char *public_key = nip46_get_public_key(bunker_client);
    if (public_key != NULL) {
        printf("Public Key: %s\n", public_key);
        free(public_key);
    } else {
        fprintf(stderr, "Failed to get public key\n");
    }

    // Clean up
    nostr_simple_pool_free(pool);
    free(bunker_client);

    return 0;
}
