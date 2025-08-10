#include "nip05.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const char *pk = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    char json[512];
    snprintf(json, sizeof(json), "{\n  \"names\": { \"_\": \"%s\" },\n  \"relays\": { \"%s\": [\"wss://r1\", \"wss://r2\"] }\n}", pk, pk);
    char *hex = NULL; char **rel = NULL; size_t n = 0; char *err = NULL;
    int rc = nostr_nip05_resolve_from_json("_", json, &hex, &rel, &n, &err);
    if (rc != 0) { fprintf(stderr, "resolve failed: %s\n", err?err:"?"); free(err); return 1; }
    if (!hex || strcmp(hex, pk) != 0) { fprintf(stderr, "pubkey mismatch\n"); free(hex); if (rel){for(size_t i=0;i<n;i++) free(rel[i]); free(rel);} return 1; }
    if (n != 2) { fprintf(stderr, "want 2 relays, got %zu\n", n); free(hex); if (rel){for(size_t i=0;i<n;i++) free(rel[i]); free(rel);} return 1; }
    if (strcmp(rel[0], "wss://r1") != 0 || strcmp(rel[1], "wss://r2") != 0) { fprintf(stderr, "relay values mismatch\n"); free(hex); for(size_t i=0;i<n;i++) free(rel[i]); free(rel); return 1; }
    free(hex); for(size_t i=0;i<n;i++) free(rel[i]); free(rel);
    return 0;
}
