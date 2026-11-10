#include "nostr/nip37/nip37.h"
#include "nostr-kinds.h"
#include "json.h"
#include "../../codec-internal.h"
#include <assert.h>
#include <stdio.h>

#define PK "1111111111111111111111111111111111111111111111111111111111111111"
static void envelope(void) {
    int64_t expiration = INT64_MAX;
    NostrEvent *ev = nostr_nip37_build_envelope(PK, 42, "opaque:identifier", 14, "ciphertext", &expiration);
    assert(ev && nostr_event_get_kind(ev) == NOSTR_KIND_DRAFT_WRAP);
    assert(!nostr_event_get_sig(ev) || !*nostr_event_get_sig(ev));
    NostrNip37Envelope parsed;
    assert(!nostr_nip37_parse_envelope(ev, &parsed));
    assert(!strcmp(parsed.identifier, "opaque:identifier") && parsed.draft_kind == 14);
    assert(parsed.has_expiration && parsed.expiration == INT64_MAX && !parsed.deleted);
    nostr_event_set_content(ev, "");
    assert(!nostr_nip37_parse_envelope(ev, &parsed) && parsed.deleted);
    NostrTag *k = nostr_tags_get(nostr_event_get_tags(ev), 1);
    const char *bad[] = {"014", "-1", "65536", "1x", "", "+14", " 14", "9223372036854775808"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        nostr_tag_set(k, 1, bad[i]);
        assert(nostr_nip37_parse_envelope(ev, &parsed) < 0);
    }
    nostr_tag_set(k, 1, "14");
    nostr_tags_append(nostr_event_get_tags(ev), nostr_tag_new("d", "duplicate", NULL));
    assert(nostr_nip37_parse_envelope(ev, &parsed) < 0);
    nostr_event_free(ev);
    ev = nostr_event_new(); nostr_event_set_kind(ev, NOSTR_KIND_DRAFT_WRAP);
    assert(nostr_nip37_parse_envelope(ev, &parsed) < 0); nostr_event_free(ev);
    assert(!nostr_nip37_build_envelope(PK, 0, "id", -1, "", NULL));
    assert(!nostr_nip37_build_envelope("invalid", 0, "id", 14, "", NULL));
    expiration = -1;
    assert(!nostr_nip37_build_envelope(PK, 0, "id", 14, "", &expiration));
}
static void drafts(void) {
    NostrEvent *draft = codec_event(14, PK, 99, nostr_tags_new(0), "quotes \" \\ \n雪");
    const char *fields[] = {"p", PK, "", "pet:name", "extension"};
    assert(!codec_tag_append(nostr_event_get_tags(draft), fields, 5));
    char *plain = nostr_nip37_serialize_draft(draft);
    assert(plain && !strstr(plain, "\"sig\"") && !strstr(plain, "\"id\""));
    NostrEvent *parsed = nostr_nip37_parse_draft(plain, strlen(plain));
    assert(parsed && !strcmp(nostr_event_get_content(parsed), nostr_event_get_content(draft)));
    assert(nostr_tag_size(nostr_tags_get(nostr_event_get_tags(parsed), 0)) == 5);
    assert(!strcmp(nostr_tag_get(nostr_tags_get(nostr_event_get_tags(parsed), 0), 2), ""));
    nostr_event_free(parsed);
    json_error_t error; json_t *root = json_loads(plain, 0, &error);
    assert(!json_object_set_new(root, "sig", json_string("")));
    char *signed_json = json_dumps(root, JSON_COMPACT);
    assert(!nostr_nip37_parse_draft(signed_json, strlen(signed_json)));
    free(signed_json); json_decref(root);
    nostr_event_set_sig(draft, "not-a-signature");
    assert(!nostr_nip37_serialize_draft(draft));
    nostr_event_free(draft); free(plain);
    const char *invalid[] = {"{}", "[]", "{", "{} garbage",
        "{\"pubkey\":\"" PK "\",\"kind\":14.0,\"created_at\":1,\"tags\":[],\"content\":\"\"}",
        "{\"pubkey\":\"" PK "\",\"kind\":14,\"created_at\":1,\"tags\":[[1]],\"content\":\"\"}",
        "{\"pubkey\":\"" PK "\",\"kind\":14,\"created_at\":1,\"tags\":[],\"content\":\"a\\u0000b\"}"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        assert(!nostr_nip37_parse_draft(invalid[i], strlen(invalid[i])));
}
static void checkpoints(void) {
    NostrEvent *ev = nostr_nip37_build_checkpoint(PK, 77, PK, "draft:with:colons", "cipher");
    assert(ev && nostr_event_get_kind(ev) == NOSTR_KIND_DRAFT_CHECKPOINT);
    NostrNip37Checkpoint parsed;
    assert(!nostr_nip37_parse_checkpoint(ev, &parsed));
    assert(!strcmp(parsed.author, PK) && !strcmp(parsed.identifier, "draft:with:colons"));
    nostr_tag_set(nostr_tags_get(nostr_event_get_tags(ev), 0), 1, "30008:" PK ":wrong");
    assert(nostr_nip37_parse_checkpoint(ev, &parsed) < 0);
    nostr_event_free(ev);
}
int main(void) {
    nostr_json_init(); envelope(); drafts(); checkpoints(); nostr_json_cleanup();
    puts("NIP-37 codecs passed"); return 0;
}
