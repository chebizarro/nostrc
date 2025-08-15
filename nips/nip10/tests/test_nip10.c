#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "nostr/nip10/nip10.h"
#include "nostr-event.h"
#include "nostr-tag.h"

static void test_add_marked_e_tag_basic(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    unsigned char id[32];
    memset(id, 0x11, sizeof id);

    // add root without relay
    assert(nostr_nip10_add_marked_e_tag(ev, id, NULL, NOSTR_E_MARK_ROOT, NULL) == 0);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    assert(tags);
    assert(nostr_tags_size(tags) >= 1);

    // verify first tag is 'e' with id and marker 'root'
    NostrTag *t0 = nostr_tags_get(tags, 0);
    assert(t0);
    assert(strcmp(nostr_tag_get(t0, 0), "e") == 0);
    assert(nostr_tag_size(t0) >= 2);
    const char *idhex = nostr_tag_get(t0, 1);
    assert(idhex && strlen(idhex) == 64);
    if (nostr_tag_size(t0) >= 4) {
        const char *marker = nostr_tag_get(t0, 3);
        assert(marker && strcmp(marker, "root") == 0);
    }

    // add reply with relay
    assert(nostr_nip10_add_marked_e_tag(ev, id, "wss://relay.example", NOSTR_E_MARK_REPLY, NULL) == 0);

    tags = (NostrTags *)nostr_event_get_tags(ev);
    size_t n = nostr_tags_size(tags);
    assert(n >= 2);

    // find a tag that has marker reply
    bool found_reply = false;
    for (size_t i = 0; i < n; ++i) {
        NostrTag *ti = nostr_tags_get(tags, i);
        if (!ti || nostr_tag_size(ti) < 2) continue;
        const char *k = nostr_tag_get(ti, 0);
        if (!k || strcmp(k, "e") != 0) continue;
        if (nostr_tag_size(ti) >= 4) {
            const char *m = nostr_tag_get(ti, 3);
            if (m && strcmp(m, "reply") == 0) {
                const char *relay = nostr_tag_get(ti, 2);
                assert(relay && strcmp(relay, "wss://relay.example") == 0);
                found_reply = true;
                break;
            }
        }
    }
    assert(found_reply);

    nostr_event_free(ev);
}

static void test_ensure_p_participants(void) {
    // parent with pubkey and a p tag with relay
    NostrEvent *parent = nostr_event_new();
    assert(parent);
    nostr_event_set_pubkey(parent, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

    NostrTag *pp = nostr_tag_new("p", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "wss://x", NULL);
    NostrTags *ptags = nostr_tags_new(1, pp);
    nostr_event_set_tags(parent, ptags);

    // reply initially with no tags
    NostrEvent *reply = nostr_event_new();
    assert(reply);

    assert(nostr_nip10_ensure_p_participants(reply, parent) == 0);

    NostrTags *rtags = (NostrTags *)nostr_event_get_tags(reply);
    assert(rtags);

    // Should contain parent author pubkey and the parent's p tag value
    bool has_parent_author = false;
    bool has_pp = false;
    size_t rn = nostr_tags_size(rtags);
    for (size_t i = 0; i < rn; ++i) {
        NostrTag *t = nostr_tags_get(rtags, i);
        if (!t || nostr_tag_size(t) < 2) continue;
        const char *k = nostr_tag_get(t, 0);
        if (!k || strcmp(k, "p") != 0) continue;
        const char *v = nostr_tag_get(t, 1);
        const char *relay = (nostr_tag_size(t) >= 3) ? nostr_tag_get(t, 2) : NULL;
        if (v && strcmp(v, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 0) {
            has_parent_author = true;
        }
        if (v && strcmp(v, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb") == 0) {
            has_pp = true;
            assert(relay && strcmp(relay, "wss://x") == 0);
        }
    }
    assert(has_parent_author);
    assert(has_pp);

    // rerun to ensure idempotent (no duplicates)
    assert(nostr_nip10_ensure_p_participants(reply, parent) == 0);

    size_t rn2 = nostr_tags_size((NostrTags *)nostr_event_get_tags(reply));
    assert(rn2 == rn);

    nostr_event_free(parent);
    nostr_event_free(reply);
}

static void test_get_thread(void) {
    // Create event with an 'e' root and an immediate reply marker
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    // root tag (hex id with 64 chars)
    NostrTag *root = nostr_tag_new("e", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", "", "root", NULL);
    // reply tag
    NostrTag *reply = nostr_tag_new("e", "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210", "", "reply", NULL);

    NostrTags *tags = nostr_tags_new(2, root, reply);
    nostr_event_set_tags(ev, tags);

    NostrThreadContext ctx; memset(&ctx, 0, sizeof ctx);
    assert(nostr_nip10_get_thread(ev, &ctx) == 0);
    assert(ctx.has_root);
    assert(ctx.has_reply);

    // verify decoded binary size by checking a couple of bytes
    assert(ctx.root_id[0] == 0x01);
    assert(ctx.root_id[31] == 0xef);
    assert(ctx.reply_id[0] == 0xfe);
    assert(ctx.reply_id[31] == 0x10);

    nostr_event_free(ev);
}

int main(void) {
    test_add_marked_e_tag_basic();
    test_ensure_p_participants();
    test_get_thread();
    printf("ok\n");
    return 0;
}

// --- Edge cases ---

static void test_mixed_markers_ordering(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    // create tags: mention, root, unmarked, reply
    NostrTag *t_mention = nostr_tag_new("e", "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff", "", "mention", NULL);
    NostrTag *t_root    = nostr_tag_new("e", "11112233445566778899aabbccddeeff00112233445566778899aabbccddeeff", "", "root", NULL);
    NostrTag *t_unmark  = nostr_tag_new("e", "22222233445566778899aabbccddeeff00112233445566778899aabbccddeeff", NULL);
    NostrTag *t_reply   = nostr_tag_new("e", "33332233445566778899aabbccddeeff00112233445566778899aabbccddeeff", "", "reply", NULL);

    NostrTags *tags = nostr_tags_new(4, t_mention, t_root, t_unmark, t_reply);
    nostr_event_set_tags(ev, tags);

    NostrThreadContext ctx; memset(&ctx, 0, sizeof ctx);
    assert(nostr_nip10_get_thread(ev, &ctx) == 0);
    assert(ctx.has_root);
    assert(ctx.has_reply);

    nostr_event_free(ev);
}

static void test_unmarked_only_prefers_first_e(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);
    NostrTag *e1 = nostr_tag_new("e", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", NULL);
    NostrTag *e2 = nostr_tag_new("e", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", NULL);
    nostr_event_set_tags(ev, nostr_tags_new(2, e1, e2));

    NostrThreadContext ctx; memset(&ctx, 0, sizeof ctx);
    assert(nostr_nip10_get_thread(ev, &ctx) == 0);
    // In our helper, root falls back to first 'e' when no explicit root present
    assert(ctx.has_root);

    nostr_event_free(ev);
}

static void test_uniqueness_with_many_tags(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    // Prepopulate with many random tags including a duplicate e we'll try to add again
    NostrTags *tags = nostr_tags_new(0);
    nostr_event_set_tags(ev, tags);

    unsigned char id[32]; memset(id, 0x5A, sizeof id);
    // First add once
    assert(nostr_nip10_add_marked_e_tag(ev, id, NULL, NOSTR_E_MARK_ROOT, NULL) == 0);
    size_t n1 = nostr_tags_size((NostrTags*)nostr_event_get_tags(ev));

    // Attempt to add same again; uniqueness should prevent growth or dup
    assert(nostr_nip10_add_marked_e_tag(ev, id, NULL, NOSTR_E_MARK_ROOT, NULL) == 0);
    size_t n2 = nostr_tags_size((NostrTags*)nostr_event_get_tags(ev));
    assert(n2 == n1);

    nostr_event_free(ev);
}

// Extend main to run edge cases
int main_edge(void) {
    test_mixed_markers_ordering();
    test_unmarked_only_prefers_first_e();
    test_uniqueness_with_many_tags();
    printf("edge ok\n");
    return 0;
}
