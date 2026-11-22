#undef NDEBUG
#include <nostr/nip78/nip78.h>
#include "nostr-kinds.h"
#include "nostr-keys.h"
#include "nostr-tag.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const char *author = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
static const char *secret = "0000000000000000000000000000000000000000000000000000000000000001";

static void roundtrip(const char *identifier, const char *content) {
    NostrEvent *event = nostr_nip78_build_event(author, identifier, content, 123);
    assert(event && !nostr_event_get_sig(event));
    assert(nostr_event_get_kind(event) == NOSTR_KIND_APPLICATION_SPECIFIC_DATA);
    assert(nostr_tags_size(nostr_event_get_tags(event)) == 1);
    NostrTag *d = nostr_tags_get(nostr_event_get_tags(event), 0);
    assert(nostr_tag_size(d) == 2 && !strcmp(nostr_tag_get(d, 0), "d"));
    NostrNip78Document *document = NULL;
    assert(nostr_nip78_parse_event(event, &document) == 0);
    char id[65];
    assert(nostr_event_compute_id(event, id) == NOSTR_EVENT_VALIDATION_OK);
    assert(!strcmp(document->event_id, id));
    assert(nostr_event_sign(event, secret) == 0);
    assert(nostr_event_check_signature(event));
    char signed_id[65];
    assert(nostr_event_compute_id(event, signed_id) == NOSTR_EVENT_VALIDATION_OK);
    assert(!strcmp(id, signed_id));
    char *json = nostr_event_serialize_compact(event);
    assert(json);
    NostrEvent *decoded = nostr_event_new();
    assert(decoded && nostr_event_deserialize_signed(decoded, json, NULL) == NOSTR_EVENT_VALIDATION_OK);
    NostrNip78Document *signed_document = NULL;
    assert(nostr_nip78_parse_event(decoded, &signed_document) == 0);
    assert(!strcmp(document->event_id, signed_document->event_id));
    nostr_nip78_document_free(signed_document);
    free(json);
    nostr_event_free(decoded);
    nostr_event_free(event);
    /* Parsed fields must remain valid after the source is freed. */
    assert(!strcmp(document->author, author));
    assert(!strcmp(document->identifier, identifier));
    assert(!strcmp(document->content, content));
    assert(document->created_at == 123);
    nostr_nip78_document_free(document);
}

static void invalid(NostrEvent *event) {
    NostrNip78Document *document = (void *)1;
    assert(nostr_nip78_parse_event(event, &document) == -EINVAL);
    assert(document == NULL);
}

static void malformed(void) {
    assert(!nostr_nip78_build_event(NULL, "d", "", 0) && errno == EINVAL);
    assert(!nostr_nip78_build_event("bad", "d", "", 0));
    assert(!nostr_nip78_build_event(author, NULL, "", 0));
    assert(!nostr_nip78_build_event(author, "d", NULL, 0));
    assert(!nostr_nip78_build_event(author, "d", "", -1));
    invalid(NULL);
    assert(nostr_nip78_parse_event(NULL, NULL) == -EINVAL);
    NostrEvent *event = nostr_nip78_build_event(author, "d", "", 0);
    assert(event);
    nostr_event_set_kind(event, 78);
    invalid(event);
    nostr_event_set_kind(event, NOSTR_KIND_APPLICATION_SPECIFIC_DATA);
    nostr_event_set_pubkey(event, "bad");
    invalid(event);
    nostr_event_set_pubkey(event, author);
    nostr_event_set_created_at(event, -1);
    invalid(event);
    nostr_event_set_created_at(event, 0);
    nostr_event_set_content(event, NULL);
    invalid(event);
    nostr_event_set_content(event, "");
    nostr_event_set_tags(event, nostr_tags_new(0));
    invalid(event); /* missing d */
    nostr_tags_append(nostr_event_get_tags(event), nostr_tag_new("d", NULL));
    invalid(event); /* short d (must not call the aborting indexed getter) */
    nostr_event_set_tags(event, nostr_tags_new(0));
    nostr_tags_append(nostr_event_get_tags(event), nostr_tag_new("d", "a", "b", NULL));
    invalid(event); /* extra d fields */
    nostr_event_set_tags(event, nostr_tags_new(0));
    nostr_tags_append(nostr_event_get_tags(event), nostr_tag_new("d", "a", NULL));
    nostr_tags_append(nostr_event_get_tags(event), nostr_tag_new("d", "a", NULL));
    invalid(event); /* even identical duplicate d tags are ambiguous */
    nostr_event_free(event);
    event = nostr_nip78_build_event(author, "d", "opaque", 0);
    nostr_tags_append(nostr_event_get_tags(event), nostr_tag_new("x", "extension", NULL));
    NostrNip78Document *document = NULL;
    assert(!nostr_nip78_parse_event(event, &document));
    nostr_nip78_document_free(document);
    nostr_event_free(event);
    nostr_nip78_document_free(NULL);
}

int main(void) {
    roundtrip("metadesk:management:v1", "ciphertext supplied by external signer");
    roundtrip("", "");
    roundtrip("app/quotes\"/unicode-\xc3\xa9", "{\"opaque\":\"line\\nvalue\"}");
    malformed();
    return 0;
}
