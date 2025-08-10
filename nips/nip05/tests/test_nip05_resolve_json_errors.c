#include "nip05.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int must_fail(const char *name, const char *json) {
    char *hex = NULL; char **rel = NULL; size_t n = 0; char *err = NULL;
    int rc = nostr_nip05_resolve_from_json(name, json, &hex, &rel, &n, &err);
    if (rc == 0) { fprintf(stderr, "unexpected success for case\n"); free(hex); if (rel){for(size_t i=0;i<n;i++) free(rel[i]); free(rel);} free(err); return 1; }
    free(err);
    return 0;
}

int main(void) {
    int rc = 0;
    // missing names
    rc |= must_fail("_", "{\"relays\":{}}\n");
    // wrong name
    rc |= must_fail("bob", "{\"names\":{\"_\":\"abcd\"}}\n");
    // invalid hex pubkey
    rc |= must_fail("_", "{\"names\":{\"_\":\"nothex\"}}\n");
    // malformed JSON (parser should fail inside helpers)
    rc |= must_fail("_", "{\"names\": \n");
    return rc;
}
