// Canonical NIP-10 example (no thin wrappers)
#include <stdio.h>
#include <string.h>

#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr/nip10/nip10.h"

static void print_hex32(const unsigned char id[32]) {
    static const char *hex = "0123456789abcdef";
    char buf[65];
    for (int i = 0; i < 32; ++i) {
        buf[2*i]   = hex[(id[i] >> 4) & 0xF];
        buf[2*i+1] = hex[id[i] & 0xF];
    }
    buf[64] = '\0';
    printf("%s", buf);
}

int main(void) {
    // Create a new event and add marked e-tags
    NostrEvent *ev = nostr_event_new();
    unsigned char root_id[32]; memset(root_id, 0x01, sizeof root_id);
    unsigned char reply_id[32]; memset(reply_id, 0x02, sizeof reply_id);

    // Add root (no relay) and reply (with relay) tags
    nostr_nip10_add_marked_e_tag(ev, root_id, NULL, NOSTR_E_MARK_ROOT, NULL);
    nostr_nip10_add_marked_e_tag(ev, reply_id, "wss://relay.example", NOSTR_E_MARK_REPLY, NULL);

    // Extract thread context
    NostrThreadContext ctx; memset(&ctx, 0, sizeof ctx);
    if (nostr_nip10_get_thread(ev, &ctx) == 0) {
        if (ctx.has_root) {
            printf("root: ");
            print_hex32(ctx.root_id);
            printf("\n");
        }
        if (ctx.has_reply) {
            printf("reply: ");
            print_hex32(ctx.reply_id);
            printf("\n");
        }
    }

    // Ensure participants on a reply event from a parent
    NostrEvent *parent = nostr_event_new();
    nostr_event_set_pubkey(parent, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    NostrTag *pt = nostr_tag_new("p", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "wss://x", NULL);
    nostr_event_set_tags(parent, nostr_tags_new(1, pt));

    NostrEvent *reply = nostr_event_new();
    nostr_nip10_ensure_p_participants(reply, parent);

    printf("participants ensured: %zu tags\n", nostr_tags_size((NostrTags*)nostr_event_get_tags(reply)));

    // Cleanup
    nostr_event_free(ev);
    nostr_event_free(parent);
    nostr_event_free(reply);
    return 0;
}
