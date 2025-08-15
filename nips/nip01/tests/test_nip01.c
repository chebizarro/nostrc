#include "nostr/nip01/nip01.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void fill32(unsigned char b[32], unsigned char v){ for (int i=0;i<32;++i) b[i]=v; }

/* Forward declaration for extra stress cycles */
__attribute__((unused)) static void run_additional_cycles(void);

int main(void){
    /* alt absent */
    NostrEvent *ev = nostr_event_new();
    char *alt = NULL; int rc = nostr_nip01_get_alt(ev, &alt);
    assert(rc != 0 && alt == NULL);

    /* add alt tag manually and read */
    NostrTags *tags = nostr_tags_new(0);
    nostr_tags_append(tags, nostr_tag_new("alt", "hello", NULL));
    nostr_event_set_tags(ev, tags);
    rc = nostr_nip01_get_alt(ev, &alt);
    assert(rc == 0 && alt && strcmp(alt, "hello") == 0);
    free(alt);

    /* add e/p/a tags */
    unsigned char id[32], pk[32]; fill32(id, 0x11); fill32(pk, 0x22);
    assert(nostr_nip01_add_e_tag(ev, id, "wss://relay", NULL) == 0);
    assert(nostr_nip01_add_p_tag(ev, pk, NULL) == 0);
    assert(nostr_nip01_add_a_tag(ev, 30000, pk, "d42", "wss://r") == 0);

    /* kind checks */
    assert(nostr_nip01_is_replaceable(0));
    assert(nostr_nip01_is_replaceable(3));
    assert(nostr_nip01_is_replaceable(10001));
    assert(!nostr_nip01_is_replaceable(1));

    assert(nostr_nip01_is_addressable(30000));
    assert(!nostr_nip01_is_addressable(29999));

    assert(nostr_nip01_is_ephemeral(20000));
    assert(!nostr_nip01_is_ephemeral(19999));

    /* filter builder */
    NostrFilterBuilder fb; assert(nostr_nip01_filter_builder_init(&fb) == 0);
    unsigned char arr_ids[2][32]; fill32(arr_ids[0], 0xAA); fill32(arr_ids[1], 0xBB);
    unsigned char arr_pks[1][32]; fill32(arr_pks[0], 0xCC);
    int kinds[2] = {1, 30000};
    assert(nostr_nip01_filter_by_ids(&fb, arr_ids, 2) == 0);
    assert(nostr_nip01_filter_by_authors(&fb, arr_pks, 1) == 0);
    assert(nostr_nip01_filter_by_kinds(&fb, kinds, 2) == 0);
    assert(nostr_nip01_filter_by_tag_e(&fb, arr_ids, 2) == 0);
    assert(nostr_nip01_filter_by_tag_p(&fb, arr_pks, 1) == 0);
    const char *arefs[1] = {"30000:deadbeef:foo"};
    assert(nostr_nip01_filter_by_tag_a(&fb, arefs, 1) == 0);
    assert(nostr_nip01_filter_since(&fb, 123) == 0);
    assert(nostr_nip01_filter_until(&fb, 456) == 0);
    assert(nostr_nip01_filter_limit(&fb, 10) == 0);
    NostrFilter f = {0};
    assert(nostr_nip01_filter_build(&fb, &f) == 0);
    /* shallow validate some fields */
    assert(f.limit == 10);
    nostr_nip01_filter_builder_dispose(&fb);
    /* free internals of the stack-allocated filter */
    nostr_filter_clear(&f);

    nostr_event_free(ev);
    /* extra cycles to stress builder ownership */
    run_additional_cycles();
    printf("test_nip01 OK\n");
    return 0;
}

/* Additional cycles to stress move/ownership semantics */
__attribute__((unused)) static void run_additional_cycles(void) {
    /* Cycle 1: minimal builder -> build -> dispose, empty filter */
    NostrFilterBuilder fb1; assert(nostr_nip01_filter_builder_init(&fb1) == 0);
    NostrFilter f1 = (NostrFilter){0};
    assert(nostr_nip01_filter_build(&fb1, &f1) == 0);
    nostr_nip01_filter_builder_dispose(&fb1);
    nostr_filter_clear(&f1);

    /* Cycle 2: with ids/authors/tags, ensure no leaks */
    NostrFilterBuilder fb2; assert(nostr_nip01_filter_builder_init(&fb2) == 0);
    unsigned char ids2[1][32]; unsigned char pks2[1][32];
    for (int i=0;i<32;++i){ ids2[0][i]=(unsigned char)i; pks2[0][i]=(unsigned char)(0xFF-i);} 
    int kinds2[3] = {1, 2, 3};
    assert(nostr_nip01_filter_by_ids(&fb2, ids2, 1) == 0);
    assert(nostr_nip01_filter_by_authors(&fb2, pks2, 1) == 0);
    assert(nostr_nip01_filter_by_kinds(&fb2, kinds2, 3) == 0);
    assert(nostr_nip01_filter_by_tag_e(&fb2, ids2, 1) == 0);
    assert(nostr_nip01_filter_by_tag_p(&fb2, pks2, 1) == 0);
    const char *aref2[1] = {"30000:deadbeef:bar"};
    assert(nostr_nip01_filter_by_tag_a(&fb2, aref2, 1) == 0);
    NostrFilter f2 = (NostrFilter){0};
    assert(nostr_nip01_filter_build(&fb2, &f2) == 0);
    nostr_nip01_filter_builder_dispose(&fb2);
    nostr_filter_clear(&f2);
}
