#include "nostr/nip01/nip01.h"
#include "nostr-event.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    NostrEvent *ev = nostr_event_new();
    if (!ev) return 1;

    unsigned char id[32] = {0};
    unsigned char pk[32] = {1};

    /* add e/p/a tags */
    if (nostr_nip01_add_e_tag(ev, id, "wss://relay.example", pk) != 0) return 2;
    if (nostr_nip01_add_p_tag(ev, pk, NULL) != 0) return 3;
    if (nostr_nip01_add_a_tag(ev, 30023, pk, "notes", NULL) != 0) return 4;

    /* kinds helpers */
    if (!nostr_nip01_is_addressable(30023)) return 5;
    if (nostr_nip01_is_ephemeral(1)) return 6;

    /* alt helper (none present) */
    char *alt = NULL;
    int rc = nostr_nip01_get_alt(ev, &alt);
    if (rc == 0) { free(alt); }

    /* filter builder */
    NostrFilterBuilder fb; 
    if (nostr_nip01_filter_builder_init(&fb) != 0) return 7;
    const unsigned char ids[][32] = {{0}};
    const int kinds[] = {1, 30023};
    nostr_nip01_filter_by_ids(&fb, ids, 1);
    nostr_nip01_filter_by_kinds(&fb, kinds, 2);
    NostrFilter f; memset(&f, 0, sizeof(f));
    if (nostr_nip01_filter_build(&fb, &f) != 0) return 8;
    nostr_nip01_filter_builder_dispose(&fb);
    /* f is an owned deep copy; allow process teardown to clean up in example */

    nostr_event_free(ev);
    return 0;
}
