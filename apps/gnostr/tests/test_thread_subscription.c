/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 gnostr contributors
 *
 * test_thread_subscription.c - Unit tests for GNostrThreadSubscription
 *
 * nostrc-pp64: Tests for the reactive thread subscription manager.
 * Validates EventBus integration, deduplication, and signal emission.
 */

#include <glib.h>
#include <nostr-gobject-1.0/gnostr-thread-subscription.h>
#include <nostr-gobject-1.0/nostr_event_bus.h>
#include "nostr-event.h"
#include "nostr-tag.h"

/* ========== Test fixtures ========== */

/* Sample 64-char hex IDs for testing */
#define ROOT_ID  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define REPLY_ID "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define REACT_ID "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
#define OTHER_ID "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"

/* Construct a minimal kind:1 NostrEvent referencing a root ID via e-tag */
static NostrEvent *make_reply_event(const char *event_id, const char *root_id) {
    NostrEvent *ev = nostr_event_new();
    ev->id = g_strdup(event_id);
    ev->pubkey = g_strdup("1111111111111111111111111111111111111111111111111111111111111111");
    ev->kind = 1;
    ev->created_at = 1700000000;
    ev->content = g_strdup("test reply");

    NostrTag *tag = nostr_tag_new("e", root_id, "", "root", NULL);
    NostrTags *tags = nostr_tags_new(0);
    nostr_tags_append(tags, tag);
    ev->tags = tags;

    return ev;
}

/* Construct a minimal kind:7 reaction NostrEvent referencing an event */
static NostrEvent *make_reaction_event(const char *event_id, const char *target_id) {
    NostrEvent *ev = nostr_event_new();
    ev->id = g_strdup(event_id);
    ev->pubkey = g_strdup("2222222222222222222222222222222222222222222222222222222222222222");
    ev->kind = 7;
    ev->created_at = 1700000001;
    ev->content = g_strdup("+");

    NostrTag *tag = nostr_tag_new("e", target_id, NULL);
    NostrTags *tags = nostr_tags_new(0);
    nostr_tags_append(tags, tag);
    ev->tags = tags;

    return ev;
}

/* Construct a minimal kind:1111 NIP-22 comment NostrEvent */
static NostrEvent *make_comment_event(const char *event_id, const char *root_id) {
    NostrEvent *ev = nostr_event_new();
    ev->id = g_strdup(event_id);
    ev->pubkey = g_strdup("3333333333333333333333333333333333333333333333333333333333333333");
    ev->kind = 1111;
    ev->created_at = 1700000002;
    ev->content = g_strdup("test comment");

    NostrTag *tag = nostr_tag_new("E", root_id, "", "root", NULL);
    NostrTags *tags = nostr_tags_new(0);
    nostr_tags_append(tags, tag);
    ev->tags = tags;

    return ev;
}

/* Signal counter context */
typedef struct {
    guint reply_count;
    guint reaction_count;
    guint comment_count;
    guint eose_count;
    NostrEvent *last_reply_event;
    NostrEvent *last_reaction_event;
    NostrEvent *last_comment_event;
} SignalCtx;

static void on_reply(GNostrThreadSubscription *sub, NostrEvent *ev, gpointer data) {
    (void)sub;
    SignalCtx *ctx = data;
    ctx->reply_count++;
    ctx->last_reply_event = ev;
}

static void on_reaction(GNostrThreadSubscription *sub, NostrEvent *ev, gpointer data) {
    (void)sub;
    SignalCtx *ctx = data;
    ctx->reaction_count++;
    ctx->last_reaction_event = ev;
}

static void on_comment(GNostrThreadSubscription *sub, NostrEvent *ev, gpointer data) {
    (void)sub;
    SignalCtx *ctx = data;
    ctx->comment_count++;
    ctx->last_comment_event = ev;
}

static void signal_ctx_clear(SignalCtx *ctx) {
    /* We don't own the events — they're borrowed from the emit call */
    memset(ctx, 0, sizeof(*ctx));
}

/* ========== Tests ========== */

static void test_new_and_properties(void) {
    g_autoptr(GNostrThreadSubscription) sub = gnostr_thread_subscription_new(ROOT_ID);
    g_assert_nonnull(sub);
    g_assert_cmpstr(gnostr_thread_subscription_get_root_id(sub), ==, ROOT_ID);
    g_assert_false(gnostr_thread_subscription_is_active(sub));
    g_assert_cmpuint(gnostr_thread_subscription_get_seen_count(sub), ==, 0);
}

static void test_start_stop(void) {
    g_autoptr(GNostrThreadSubscription) sub = gnostr_thread_subscription_new(ROOT_ID);

    gnostr_thread_subscription_start(sub);
    g_assert_true(gnostr_thread_subscription_is_active(sub));

    /* Double start is a no-op */
    gnostr_thread_subscription_start(sub);
    g_assert_true(gnostr_thread_subscription_is_active(sub));

    gnostr_thread_subscription_stop(sub);
    g_assert_false(gnostr_thread_subscription_is_active(sub));

    /* Double stop is a no-op */
    gnostr_thread_subscription_stop(sub);
    g_assert_false(gnostr_thread_subscription_is_active(sub));

}

static void test_reply_signal_via_eventbus(void) {
    g_autoptr(GNostrThreadSubscription) sub = gnostr_thread_subscription_new(ROOT_ID);
    SignalCtx ctx = {0};

    g_signal_connect(sub, "reply-received", G_CALLBACK(on_reply), &ctx);
    gnostr_thread_subscription_start(sub);

    /* Emit a kind:1 event that references our root */
    NostrEvent *ev = make_reply_event(REPLY_ID, ROOT_ID);
    GNostrEventBus *bus = gnostr_event_bus_get_default();
    gnostr_event_bus_emit(bus, "event::kind::1", ev);

    g_assert_cmpuint(ctx.reply_count, ==, 1);
    g_assert_nonnull(ctx.last_reply_event);

    /* Verify deduplication: same event again should not fire */
    gnostr_event_bus_emit(bus, "event::kind::1", ev);
    g_assert_cmpuint(ctx.reply_count, ==, 1);

    g_assert_cmpuint(gnostr_thread_subscription_get_seen_count(sub), ==, 1);

    nostr_event_free(ev);
    signal_ctx_clear(&ctx);
}

static void test_reaction_signal_via_eventbus(void) {
    g_autoptr(GNostrThreadSubscription) sub = gnostr_thread_subscription_new(ROOT_ID);
    SignalCtx ctx = {0};

    g_signal_connect(sub, "reaction-received", G_CALLBACK(on_reaction), &ctx);
    gnostr_thread_subscription_start(sub);

    NostrEvent *ev = make_reaction_event(REACT_ID, ROOT_ID);
    GNostrEventBus *bus = gnostr_event_bus_get_default();
    gnostr_event_bus_emit(bus, "event::kind::7", ev);

    g_assert_cmpuint(ctx.reaction_count, ==, 1);
    g_assert_nonnull(ctx.last_reaction_event);

    nostr_event_free(ev);
    signal_ctx_clear(&ctx);
}

static void test_comment_signal_via_eventbus(void) {
    g_autoptr(GNostrThreadSubscription) sub = gnostr_thread_subscription_new(ROOT_ID);
    SignalCtx ctx = {0};

    g_signal_connect(sub, "comment-received", G_CALLBACK(on_comment), &ctx);
    gnostr_thread_subscription_start(sub);

    /* NIP-22 comment with uppercase E tag */
    NostrEvent *ev = make_comment_event(
        "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
        ROOT_ID);
    GNostrEventBus *bus = gnostr_event_bus_get_default();
    gnostr_event_bus_emit(bus, "event::kind::1111", ev);

    g_assert_cmpuint(ctx.comment_count, ==, 1);

    nostr_event_free(ev);
    signal_ctx_clear(&ctx);
}

static void test_unrelated_event_filtered(void) {
    g_autoptr(GNostrThreadSubscription) sub = gnostr_thread_subscription_new(ROOT_ID);
    SignalCtx ctx = {0};

    g_signal_connect(sub, "reply-received", G_CALLBACK(on_reply), &ctx);
    gnostr_thread_subscription_start(sub);

    /* Event referencing a different root - should be filtered */
    NostrEvent *ev = make_reply_event(REPLY_ID, OTHER_ID);
    GNostrEventBus *bus = gnostr_event_bus_get_default();
    gnostr_event_bus_emit(bus, "event::kind::1", ev);

    g_assert_cmpuint(ctx.reply_count, ==, 0);

    nostr_event_free(ev);
    signal_ctx_clear(&ctx);
}

static void test_add_monitored_id(void) {
    g_autoptr(GNostrThreadSubscription) sub = gnostr_thread_subscription_new(ROOT_ID);
    SignalCtx ctx = {0};

    g_signal_connect(sub, "reply-received", G_CALLBACK(on_reply), &ctx);
    gnostr_thread_subscription_start(sub);

    /* Event referencing a mid-thread ID (not root) */
    char *mid_id = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    NostrEvent *ev = make_reply_event(REPLY_ID, mid_id);
    GNostrEventBus *bus = gnostr_event_bus_get_default();

    /* Should be filtered initially */
    gnostr_event_bus_emit(bus, "event::kind::1", ev);
    g_assert_cmpuint(ctx.reply_count, ==, 0);

    /* Add the mid-thread ID to monitored set */
    gnostr_thread_subscription_add_monitored_id(sub, mid_id);

    /* Now it should match */
    /* Need a different event ID since the first one was already seen in the filter */
    nostr_event_free(ev);
    ev = make_reply_event(
        "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef",
        mid_id);
    gnostr_event_bus_emit(bus, "event::kind::1", ev);
    g_assert_cmpuint(ctx.reply_count, ==, 1);

    nostr_event_free(ev);
    signal_ctx_clear(&ctx);
}

static void test_no_signals_after_stop(void) {
    g_autoptr(GNostrThreadSubscription) sub = gnostr_thread_subscription_new(ROOT_ID);
    SignalCtx ctx = {0};

    g_signal_connect(sub, "reply-received", G_CALLBACK(on_reply), &ctx);
    gnostr_thread_subscription_start(sub);
    gnostr_thread_subscription_stop(sub);

    /* After stop, events should not trigger signals */
    NostrEvent *ev = make_reply_event(REPLY_ID, ROOT_ID);
    GNostrEventBus *bus = gnostr_event_bus_get_default();
    gnostr_event_bus_emit(bus, "event::kind::1", ev);

    g_assert_cmpuint(ctx.reply_count, ==, 0);

    nostr_event_free(ev);
    signal_ctx_clear(&ctx);
}

/* ========== Main ========== */

int main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/thread-subscription/new-and-properties", test_new_and_properties);
    g_test_add_func("/thread-subscription/start-stop", test_start_stop);
    g_test_add_func("/thread-subscription/reply-signal", test_reply_signal_via_eventbus);
    g_test_add_func("/thread-subscription/reaction-signal", test_reaction_signal_via_eventbus);
    g_test_add_func("/thread-subscription/comment-signal", test_comment_signal_via_eventbus);
    g_test_add_func("/thread-subscription/unrelated-filtered", test_unrelated_event_filtered);
    g_test_add_func("/thread-subscription/add-monitored-id", test_add_monitored_id);
    g_test_add_func("/thread-subscription/no-signals-after-stop", test_no_signals_after_stop);

    return g_test_run();
}
