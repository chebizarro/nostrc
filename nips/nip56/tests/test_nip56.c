#include "nostr/nip56/nip56.h"
#include "nostr-kinds.h"
#include "json.h"
#include "codec-internal.h"
#include <assert.h>
#include <stdio.h>
#define PK "1111111111111111111111111111111111111111111111111111111111111111"
#define ID "2222222222222222222222222222222222222222222222222222222222222222"
int main(void) {
    nostr_json_init();
    for (int c = NOSTR_NIP56_NUDITY; c <= NOSTR_NIP56_OTHER; c++) {
        NostrNip56Category parsed;
        assert(!nostr_nip56_category_parse(nostr_nip56_category_name((NostrNip56Category)c), &parsed));
        assert(parsed == c);
    }
    NostrNip56Category c;
    assert(nostr_nip56_category_parse("Spam", &c) < 0);
    assert(nostr_nip56_category_parse("unknown", &c) < 0);
    assert(!nostr_nip56_category_name((NostrNip56Category)-1));
    NostrNip56Target targets[] = {
        {NOSTR_NIP56_ACCOUNT, PK, NOSTR_NIP56_SPAM},
        {NOSTR_NIP56_EVENT, ID, NOSTR_NIP56_MALWARE},
        {NOSTR_NIP56_BLOB, PK, NOSTR_NIP56_MALWARE}
    }, out[3];
    NostrEvent *ev = nostr_nip56_build_unsigned(PK, 1, targets, 1, false, "User-written \"explanation\"\n");
    assert(ev && nostr_event_get_kind(ev) == NOSTR_KIND_REPORTING);
    assert(!nostr_event_get_sig(ev) || !*nostr_event_get_sig(ev));
    assert(!nostr_nip56_validate(ev, false));
    assert(nostr_nip56_validate(ev, true) < 0);
    char *wire = nostr_event_serialize(ev);
    assert(wire && strstr(wire, "spam") && !strstr(wire, "\"e\""));
    free(wire); nostr_event_free(ev);
    assert(!nostr_nip56_build_unsigned(PK, 1, targets, 1, true, NULL));
    ev = nostr_nip56_build_unsigned(PK, 1, targets, 3, true, NULL);
    assert(ev && !nostr_nip56_validate(ev, true));
    size_t n;
    assert(nostr_nip56_parse(ev, true, out, 2, &n) == -ENOSPC && n == 3);
    assert(!nostr_nip56_parse(ev, true, out, 3, &n) && n == 3);
    assert(out[2].type == NOSTR_NIP56_BLOB && out[2].category == NOSTR_NIP56_MALWARE);
    NostrTag *tag = nostr_tags_get(nostr_event_get_tags(ev), 0);
    nostr_tag_set(tag, 2, "invalid"); assert(nostr_nip56_validate(ev, false) < 0);
    nostr_tag_set(tag, 2, "spam"); nostr_tag_set(tag, 1, "bad-key");
    assert(nostr_nip56_validate(ev, false) < 0); nostr_event_free(ev);
    targets[1].category = NOSTR_NIP56_SPAM;
    assert(!nostr_nip56_build_unsigned(PK, 1, targets, 3, true, NULL)); /* x/e mismatch */
    assert(!nostr_nip56_build_unsigned(PK, 1, targets + 1, 1, true, NULL)); /* no p */
    targets[0].id = "invalid";
    assert(!nostr_nip56_build_unsigned(PK, 1, targets, 1, false, NULL));
    targets[0].id = PK; targets[0].category = (NostrNip56Category)9;
    assert(!nostr_nip56_build_unsigned(PK, 1, targets, 1, false, NULL));
    ev = codec_event(NOSTR_KIND_REPORTING, PK, 1, nostr_tags_new(0), "");
    const char *untyped[] = {"p", PK};
    assert(!codec_tag_append(nostr_event_get_tags(ev), untyped, 2));
    assert(nostr_nip56_validate(ev, false) < 0); nostr_event_free(ev);
    nostr_json_cleanup(); puts("NIP-56 codecs passed"); return 0;
}
