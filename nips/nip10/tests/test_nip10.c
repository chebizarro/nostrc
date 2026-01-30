#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "nostr/nip10/nip10.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include "json.h"  /* for nostr_event_deserialize */

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

int main_edge(void);  /* Forward declaration */

int main(void) {
    test_add_marked_e_tag_basic();
    test_ensure_p_participants();
    test_get_thread();
    printf("ok\n");

    /* Also run edge cases including marker-at-index-3 tests */
    return main_edge();
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

/* Test for the exact Damus Notedeck format that was failing:
 * ["e", "id", "", "root"] - marker at index 3 with empty relay URL at index 2
 * This is the format used in event 2a86231597155dbe4149a503b62f44f5d042c8f39296377979c1acb7fab4b1d4
 */
static void test_damus_notedeck_format(void) {
    /* This is the exact problematic JSON structure from the bug report */
    const char *json =
        "{"
        "\"id\":\"2a86231597155dbe4149a503b62f44f5d042c8f39296377979c1acb7fab4b1d4\","
        "\"tags\":["
            "[\"client\",\"Damus Notedeck\"],"
            "[\"e\",\"816fe9fc80ab2a5a3888220414874cdc76fa430118e4344e93df089f155c6abd\",\"\",\"root\"],"
            "[\"e\",\"dfc9092b7d3c430430a30772c1bb8cf5a0e84aeb776e223fdbb575c774afe870\",\"\",\"reply\"],"
            "[\"p\",\"958b754a1d3de5b5eca0fe31d2d555f451325f8498a83da1997b7fcd5c39e88c\"],"
            "[\"p\",\"32e1827635450ebb3c5a7d12c1f8e7b2b514439ac10a67eef3d9fd9c5c68e245\"]"
        "]"
        "}";

    NostrEvent *ev = nostr_event_new();
    assert(ev);
    int rc = nostr_event_deserialize(ev, json);
    /* Return value: compact path returns 0 on success, backend returns 1 */
    assert(rc == 0 || rc == 1);

    /* Now verify that NIP-10 parsing works correctly */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    assert(tags);

    /* Tag at index 1 should be the root e-tag with 4 elements */
    NostrTag *root_tag = nostr_tags_get(tags, 1);
    assert(nostr_tag_size(root_tag) == 4);
    assert(strcmp(nostr_tag_get(root_tag, 3), "root") == 0);

    /* Tag at index 2 should be the reply e-tag with 4 elements */
    NostrTag *reply_tag = nostr_tags_get(tags, 2);
    assert(nostr_tag_size(reply_tag) == 4);
    assert(strcmp(nostr_tag_get(reply_tag, 3), "reply") == 0);

    nostr_event_free(ev);
    printf("test_damus_notedeck_format: ok\n");
}

/* Test that NostrEvent deserialize properly parses all 4 tag elements including marker */
static void test_deserialize_preserves_tag_markers(void) {
    const char *json =
        "{"
        "\"tags\":["
            "[\"e\",\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"wss://relay.example\",\"root\"],"
            "[\"e\",\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"\",\"reply\"]"
        "]"
        "}";

    NostrEvent *ev = nostr_event_new();
    assert(ev);
    int rc = nostr_event_deserialize(ev, json);
    /* Return value: compact path returns 0 on success, backend returns 1 */
    assert(rc == 0 || rc == 1);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    assert(tags);
    assert(nostr_tags_size(tags) == 2);

    /* First tag: ["e", "aaa...", "wss://relay.example", "root"] */
    NostrTag *t0 = nostr_tags_get(tags, 0);
    assert(nostr_tag_size(t0) == 4);
    assert(strcmp(nostr_tag_get(t0, 0), "e") == 0);
    assert(strcmp(nostr_tag_get(t0, 2), "wss://relay.example") == 0);
    assert(strcmp(nostr_tag_get(t0, 3), "root") == 0);

    /* Second tag: ["e", "bbb...", "", "reply"] */
    NostrTag *t1 = nostr_tags_get(tags, 1);
    assert(nostr_tag_size(t1) == 4);
    assert(strcmp(nostr_tag_get(t1, 0), "e") == 0);
    assert(strcmp(nostr_tag_get(t1, 2), "") == 0);
    assert(strcmp(nostr_tag_get(t1, 3), "reply") == 0);

    nostr_event_free(ev);
    printf("test_deserialize_preserves_tag_markers: ok\n");
}

// --- NostrNip10ThreadInfo tests ---

static void test_thread_info_parse_with_markers(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    // Create event with explicit root and reply markers
    NostrTag *root = nostr_tag_new("e", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "", "root", NULL);
    NostrTag *reply = nostr_tag_new("e", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "", "reply", NULL);
    NostrTags *tags = nostr_tags_new(2, root, reply);
    nostr_event_set_tags(ev, tags);

    NostrNip10ThreadInfo info;
    memset(&info, 0, sizeof(info));

    int rc = nostr_nip10_parse_thread_from_event(ev, &info);
    assert(rc == 0);
    assert(info.root_id != NULL);
    assert(info.reply_id != NULL);
    assert(strcmp(info.root_id, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 0);
    assert(strcmp(info.reply_id, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb") == 0);

    nostr_nip10_thread_info_clear(&info);
    assert(info.root_id == NULL);
    assert(info.reply_id == NULL);

    nostr_event_free(ev);
}

static void test_thread_info_parse_positional_fallback(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    // Create event with no markers (legacy positional style)
    NostrTag *e1 = nostr_tag_new("e", "1111111111111111111111111111111111111111111111111111111111111111", NULL);
    NostrTag *e2 = nostr_tag_new("e", "2222222222222222222222222222222222222222222222222222222222222222", NULL);
    NostrTags *tags = nostr_tags_new(2, e1, e2);
    nostr_event_set_tags(ev, tags);

    NostrNip10ThreadInfo info;
    memset(&info, 0, sizeof(info));

    int rc = nostr_nip10_parse_thread_from_event(ev, &info);
    assert(rc == 0);
    // First e-tag should be root
    assert(info.root_id != NULL);
    assert(strcmp(info.root_id, "1111111111111111111111111111111111111111111111111111111111111111") == 0);
    // Last e-tag should be reply
    assert(info.reply_id != NULL);
    assert(strcmp(info.reply_id, "2222222222222222222222222222222222222222222222222222222222222222") == 0);

    nostr_nip10_thread_info_clear(&info);
    nostr_event_free(ev);
}

static void test_thread_info_clear_null_safe(void) {
    // Should not crash on NULL
    nostr_nip10_thread_info_clear(NULL);

    // Should handle info with NULL members
    NostrNip10ThreadInfo info;
    info.root_id = NULL;
    info.reply_id = NULL;
    nostr_nip10_thread_info_clear(&info);
    assert(info.root_id == NULL);
    assert(info.reply_id == NULL);
}

/* Test NIP-10: When only root marker exists (no reply marker), this is a
 * direct reply to the root event. Reply_id should equal root_id.
 * This matches the exact format from the bug report. */
static void test_root_only_marker_direct_reply(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    /* Create event with ONLY "root" marker e-tag and a "mention" p-tag
     * This is a direct reply to the root event per NIP-10 */
    NostrTag *root = nostr_tag_new("e", "fb7f47f41033757003c892045e58bd2cf3bcb0ec7a171ae3867203d41f4a0ed0", "wss://nostr.mom", "root", NULL);
    NostrTag *mention_p = nostr_tag_new("p", "1bc70a0148b3f316da33fe3c89f23e3e71ac4ff998027ec712b905cd24f6a411", "", "mention", NULL);
    NostrTags *tags = nostr_tags_new(2, root, mention_p);
    nostr_event_set_tags(ev, tags);

    NostrNip10ThreadInfo info;
    memset(&info, 0, sizeof(info));

    int rc = nostr_nip10_parse_thread_from_event(ev, &info);
    assert(rc == 0);

    /* Both root_id and reply_id should be set to the root event */
    assert(info.root_id != NULL);
    assert(info.reply_id != NULL);
    assert(strcmp(info.root_id, "fb7f47f41033757003c892045e58bd2cf3bcb0ec7a171ae3867203d41f4a0ed0") == 0);
    /* Per NIP-10: when there's only root and no reply marker, reply_id = root_id */
    assert(strcmp(info.reply_id, "fb7f47f41033757003c892045e58bd2cf3bcb0ec7a171ae3867203d41f4a0ed0") == 0);

    nostr_nip10_thread_info_clear(&info);
    nostr_event_free(ev);
    printf("test_root_only_marker_direct_reply: ok\n");
}

// Extend main to run edge cases
int main_edge(void) {
    test_mixed_markers_ordering();
    test_unmarked_only_prefers_first_e();
    test_uniqueness_with_many_tags();
    // nostrc-sx2: Damus Notedeck marker-at-index-3 format tests
    test_damus_notedeck_format();
    test_deserialize_preserves_tag_markers();
    test_thread_info_parse_with_markers();
    test_thread_info_parse_positional_fallback();
    test_thread_info_clear_null_safe();
    // nostrc-mef: Root-only marker with no reply marker should set reply_id = root_id
    test_root_only_marker_direct_reply();
    printf("edge ok\n");
    return 0;
}
