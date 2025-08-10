#include "nip05.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const char *json = "{\n"
                       "  \"names\": { \"_\": \"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789\" }\n"
                       "}";
    char *hex = NULL; char **rel = NULL; size_t n = 0; char *err = NULL;
    int rc = nostr_nip05_resolve_from_json("_", json, &hex, &rel, &n, &err);
    if (rc != 0) { fprintf(stderr, "resolve failed: %s\n", err?err:"?"); free(err); return 1; }
    if (!hex) { fprintf(stderr, "no hex returned\n"); return 1; }
    if (n != 0 || rel != NULL) { fprintf(stderr, "unexpected relays returned\n"); free(hex); if (rel){for(size_t i=0;i<n;i++) free(rel[i]); free(rel);} return 1; }
    free(hex);
    return 0;
}
