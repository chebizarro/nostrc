#include "nostr/nip46/nip46_msg.h"
#include "nostr/nip46/nip46_uri.h"
#include <assert.h>
#include <jansson.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *PK =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

static void test_uri_negatives(void) {
    char uri[32768];
    NostrNip46BunkerURI bunker;
    snprintf(uri, sizeof(uri), "bunker://%s?secret=one&secret=two", PK);
    assert(nostr_nip46_uri_parse_bunker(uri, &bunker) != 0);
    assert(!bunker.remote_signer_pubkey_hex && !bunker.secret && !bunker.relays);

    snprintf(uri, sizeof(uri), "bunker://%s?secret=abc%%00def", PK);
    assert(nostr_nip46_uri_parse_bunker(uri, &bunker) != 0);
    snprintf(uri, sizeof(uri), "bunker://%s?secret=abc%%Q0", PK);
    assert(nostr_nip46_uri_parse_bunker(uri, &bunker) != 0);
    snprintf(uri, sizeof(uri), "bunker://%s?relay=wss%%3A%%2F%%2Fnos.lol&relay=wss%%3A%%2F%%2Fnos.lol", PK);
    assert(nostr_nip46_uri_parse_bunker(uri, &bunker) != 0);
    snprintf(uri, sizeof(uri), "bunker://%s?secret=", PK);
    assert(nostr_nip46_uri_parse_bunker(uri, &bunker) != 0);

    size_t used = (size_t)snprintf(uri, sizeof(uri), "bunker://%s?", PK);
    for (int i = 0; i < 17; i++)
        used += (size_t)snprintf(uri + used, sizeof(uri) - used,
            "%srelay=wss%%3A%%2F%%2Fr%d.example", i ? "&" : "", i);
    assert(nostr_nip46_uri_parse_bunker(uri, &bunker) != 0);
}

static void test_uri_positive(void) {
    char uri[1024];
    NostrNip46BunkerURI bunker;
    snprintf(uri, sizeof(uri),
        "bunker://%s?relay=wss%%3A%%2F%%2Fnos.lol&relay=wss%%3A%%2F%%2Frelay.nostr.band&secret=a%%20b",
        PK);
    assert(nostr_nip46_uri_parse_bunker(uri, &bunker) == 0);
    assert(bunker.n_relays == 2);
    assert(!strcmp(bunker.relays[0], "wss://nos.lol"));
    assert(!strcmp(bunker.relays[1], "wss://relay.nostr.band"));
    assert(!strcmp(bunker.secret, "a b"));
    nostr_nip46_uri_bunker_free(&bunker);
}

static void test_message_hardening(void) {
    NostrNip46Request request;
    NostrNip46Response response;
    assert(nostr_nip46_request_parse(
        "{\"id\":\"a\",\"id\":\"b\",\"method\":\"ping\",\"params\":[]}",
        &request) != 0);
    assert(nostr_nip46_response_parse(
        "{\"id\":\"a\",\"result\":\"ok\",\"error\":\"bad\"}",
        &response) == 0);
    assert(!strcmp(response.result, "ok") && !strcmp(response.error, "bad"));
    nostr_nip46_response_free(&response);

    const char *id = "x\"\\\n";
    const char *error = "denied \"quoted\"\\line\nnext";
    char *encoded = nostr_nip46_response_build_err(id, error);
    assert(encoded);
    assert(nostr_nip46_response_parse(encoded, &response) == 0);
    assert(!strcmp(response.id, id));
    assert(!strcmp(response.error, error));
    nostr_nip46_response_free(&response);
    free(encoded);

    encoded = nostr_nip46_response_build_ok(id, "{\"kind\":1}");
    assert(encoded);
    assert(nostr_nip46_response_parse(encoded, &response) == 0);
    assert(!strcmp(response.id, id));
    assert(!strcmp(response.result, "{\"kind\":1}"));
    nostr_nip46_response_free(&response);
    free(encoded);
}

static void test_random_ids(void) {
    enum { COUNT = 512 };
    char *ids[COUNT] = {0};
    for (size_t i = 0; i < COUNT; i++) {
        ids[i] = nostr_nip46_request_id_generate();
        assert(ids[i] && strlen(ids[i]) == 64);
        for (size_t j = 0; j < 64; j++)
            assert(isdigit((unsigned char)ids[i][j]) ||
                   (ids[i][j] >= 'a' && ids[i][j] <= 'f'));
        for (size_t j = 0; j < i; j++) assert(strcmp(ids[i], ids[j]) != 0);
    }
    for (size_t i = 0; i < COUNT; i++) free(ids[i]);
}

/* Jansson's set_new transfers ownership even when insertion fails. Exercise
 * allocator failures so error cleanup remains safe, not just the happy path. */
static int allocations_left;
static void *limited_malloc(size_t size) {
    if (allocations_left-- == 0) return NULL;
    return malloc(size);
}

static void test_allocation_failures(void) {
    for (int i = 0; i < 80; i++) {
        allocations_left = i;
        json_set_alloc_funcs(limited_malloc, free);
        char *text = nostr_nip46_response_build_ok("id", "{\"kind\":1}");
        free(text);
        allocations_left = i;
        const char *params[] = { "one", "two" };
        text = nostr_nip46_request_build("id", "ping", params, 2);
        free(text);
        json_set_alloc_funcs(malloc, free);
    }
    NostrNip46Request partial = {0};
    partial.n_params = 2;
    nostr_nip46_request_free(&partial);
    assert(partial.n_params == 0);
}

int main(void) {
    test_uri_negatives();
    test_uri_positive();
    test_message_hardening();
    test_random_ids();
    test_allocation_failures();
    puts("test_caller_hardening: OK");
    return 0;
}
