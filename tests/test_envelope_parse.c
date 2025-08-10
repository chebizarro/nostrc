#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nostr-envelope.h"

static void expect_parse_null(const char *msg) {
    NostrEnvelope *e = nostr_envelope_parse(msg);
    assert(e == NULL);
}

static void test_malformed_inputs(void) {
    // Null / empty
    expect_parse_null(NULL);
    expect_parse_null("");
    expect_parse_null("not json");

    // Missing bracket
    expect_parse_null("\"EVENT\"");

    // EVENT with missing fields
    expect_parse_null("[\"EVENT\"]");
    expect_parse_null("[\"EVENT\",\"sub\"]");
    // Bad JSON object for event (unterminated)
    expect_parse_null("[\"EVENT\",\"sub\",{");

    // EOSE without id
    expect_parse_null("[\"EOSE\"]");

    // CLOSED missing reason
    expect_parse_null("[\"CLOSED\",\"sub\"]");

    // OK malformed boolean
    expect_parse_null("[\"OK\",\"id\", maybe]");
}

static void test_minimal_valids(void) {
    // Minimal EOSE with id
    NostrEnvelope *e1 = nostr_envelope_parse("[\"EOSE\",\"sub\"]");
    assert(e1 && e1->type == NOSTR_ENVELOPE_EOSE);
    nostr_envelope_free(e1);

    // Minimal NOTICE
    NostrEnvelope *e2 = nostr_envelope_parse("[\"NOTICE\",\"hello\"]");
    assert(e2 && e2->type == NOSTR_ENVELOPE_NOTICE);
    nostr_envelope_free(e2);

    // Minimal CLOSED
    NostrEnvelope *e3 = nostr_envelope_parse("[\"CLOSED\",\"sub\",\"bye\"]");
    assert(e3 && e3->type == NOSTR_ENVELOPE_CLOSED);
    nostr_envelope_free(e3);
}

int main(void) {
    test_malformed_inputs();
    test_minimal_valids();
    printf("test_envelope_parse: OK\n");
    return 0;
}
