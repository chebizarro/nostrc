#include "nip05.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int expect_parse(const char *in, const char *want_name, const char *want_domain) {
    char *name = NULL, *domain = NULL;
    if (nostr_nip05_parse_identifier(in, &name, &domain) != 0) {
        fprintf(stderr, "parse failed for %s\n", in);
        return 1;
    }
    int rc = 0;
    if (strcmp(name, want_name) != 0) { fprintf(stderr, "name mismatch: got %s want %s\n", name, want_name); rc = 1; }
    if (strcmp(domain, want_domain) != 0) { fprintf(stderr, "domain mismatch: got %s want %s\n", domain, want_domain); rc = 1; }
    free(name); free(domain);
    return rc;
}

int main(void) {
    int rc = 0;
    rc |= expect_parse("_@Example.COM", "_", "example.com");
    rc |= expect_parse("AlIce@sub.Domain.io", "alice", "sub.domain.io");
    rc |= expect_parse("example.org", "_", "example.org");

    // Invalid inputs should fail
    char *n=NULL,*d=NULL;
    if (nostr_nip05_parse_identifier("not an id", &n, &d) == 0) { fprintf(stderr, "invalid accepted\n"); free(n); free(d); rc |= 1; }
    return rc;
}
