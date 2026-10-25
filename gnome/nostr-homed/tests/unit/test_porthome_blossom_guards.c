/*
 * test_porthome_blossom_guards.c — offline sanity checks for the
 * libblossom pool's URL/size guards, done without any network.
 * End-to-end round-trip is covered by tests/integration/test_porthome_smallhome.py.
 */

#include <blossom/pool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); return 1; } while (0)
#define ASSERT(cond) do { if (!(cond)) FAIL("%s:%d %s", __FILE__, __LINE__, #cond); } while (0)

static int test_url_ok(void) {
    /* Accepted. */
    ASSERT(blossom_pool_url_ok("https://blossom.example.com") == 0);
    ASSERT(blossom_pool_url_ok("https://blossom.example.com:8443") == 0);

    /* Refused. */
    ASSERT(blossom_pool_url_ok(NULL) != 0);
    ASSERT(blossom_pool_url_ok("") != 0);
    ASSERT(blossom_pool_url_ok("http://blossom.example.com") != 0); /* not https */
    ASSERT(blossom_pool_url_ok("https://") != 0);                    /* no host */
    ASSERT(blossom_pool_url_ok("https://localhost/base") != 0);      /* loopback name */
    ASSERT(blossom_pool_url_ok("https://127.0.0.1") != 0);           /* loopback literal */
    ASSERT(blossom_pool_url_ok("https://[::1]") != 0);
    ASSERT(blossom_pool_url_ok("https://user@host") != 0);           /* userinfo */
    ASSERT(blossom_pool_url_ok("https://host/") != 0);               /* trailing / */
    ASSERT(blossom_pool_url_ok("https://host/path#frag") != 0);
    ASSERT(blossom_pool_url_ok("https://host with space") != 0);
    return 0;
}

static int test_client_new_rejects_bad_urls(void) {
    /* With allow_insecure unset, new() must refuse non-HTTPS URLs. */
    const char *bad_servers[] = { "https://s1.example.com", "http://s2.example.com" };
    blossom_pool_opts_t opts = {0};
    opts.servers = bad_servers;
    opts.n_servers = 2;
    blossom_pool_t *c = NULL;
    int rc = blossom_pool_new(&opts, NULL, &c);
    ASSERT(rc != 0);
    ASSERT(c == NULL);
    return 0;
}

static int test_client_new_accepts_https(void) {
    const char *servers[] = { "https://s1.example.com", "https://s2.example.com" };
    blossom_pool_opts_t opts = {0};
    opts.servers = servers;
    opts.n_servers = 2;
    blossom_pool_t *c = NULL;
    ASSERT(blossom_pool_new(&opts, NULL, &c) == 0);
    ASSERT(c != NULL);
    blossom_pool_free(c);
    return 0;
}

static int test_client_size_cap_rejects_large_upload(void) {
    const char *servers[] = { "https://s1.example.com" };
    blossom_pool_opts_t opts = {0};
    opts.servers = servers;
    opts.n_servers = 1;
    opts.max_blob_bytes = 128;
    blossom_pool_t *c = NULL;
    ASSERT(blossom_pool_new(&opts, NULL, &c) == 0);

    uint8_t big[256]; memset(big, 0xAB, sizeof(big));
    char hex[65] = {0};
    int rc = blossom_pool_upload(c, big, sizeof(big), NULL, hex);
    ASSERT(rc == BLOSSOM_POOL_ERR_TOO_LARGE);

    blossom_pool_free(c);
    return 0;
}

static int test_fetch_rejects_bad_hex(void) {
    const char *servers[] = { "https://s1.example.com" };
    blossom_pool_opts_t opts = {0};
    opts.servers = servers; opts.n_servers = 1;
    blossom_pool_t *c = NULL;
    ASSERT(blossom_pool_new(&opts, NULL, &c) == 0);
    uint8_t *out = NULL; size_t olen = 0;
    ASSERT(blossom_pool_fetch(c, "not-hex", &out, &olen) != 0);
    ASSERT(out == NULL);
    ASSERT(blossom_pool_fetch(c, "0", &out, &olen) != 0);
    ASSERT(blossom_pool_fetch(c,
        "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz",
        &out, &olen) != 0);
    blossom_pool_free(c);
    return 0;
}

int main(void) {
    if (test_url_ok()) return 1;
    if (test_client_new_rejects_bad_urls()) return 1;
    if (test_client_new_accepts_https()) return 1;
    if (test_client_size_cap_rejects_large_upload()) return 1;
    if (test_fetch_rejects_bad_hex()) return 1;
    printf("OK porthome_blossom_guards\n");
    return 0;
}
