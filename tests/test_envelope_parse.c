#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "envelope.h"

static void expect_parse_null(const char *msg) {
    Envelope *e = parse_message(msg);
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
    Envelope *e1 = parse_message("[\"EOSE\",\"sub\"]");
    assert(e1 && e1->type == ENVELOPE_EOSE);
    free_envelope(e1);

    // Minimal NOTICE
    Envelope *e2 = parse_message("[\"NOTICE\",\"hello\"]");
    assert(e2 && e2->type == ENVELOPE_NOTICE);
    free_envelope(e2);

    // Minimal CLOSED
    Envelope *e3 = parse_message("[\"CLOSED\",\"sub\",\"bye\"]");
    assert(e3 && e3->type == ENVELOPE_CLOSED);
    free_envelope(e3);
}

int main(void) {
    test_malformed_inputs();
    test_minimal_valids();
    printf("test_envelope_parse: OK\n");
    return 0;
}
