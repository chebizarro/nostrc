/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 gnostr contributors
 *
 * test_thread_subscription.c - Unit tests for GnostrThreadSubscription
 *
 * nostrc-pp64: Tests for the reactive thread subscription manager.
 * Validates EventBus integration, deduplication, and signal emission.
 */

#include <glib.h>
#include "../src/model/gnostr-thread-subscription.h"
#include "nostr_event_bus.h"

/* ========== Test fixtures ========== */

/* Sample 64-char hex IDs for testing */
#define ROOT_ID  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define REPLY_ID "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define REACT_ID "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
#define OTHER_ID "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"

/* Construct a minimal kind:1 event JSON referencing a root ID */
static char *make_reply_json(const char *event_id, const char *root_id) {
    return g_strdup_printf(
        "{\"id\":\"%s\",\"pubkey\":\"1111111111111111111111111111111111111111111111111111111111111111\","
        "\"kind\":1,\"created_at\":1700000000,\"content\":\"test reply\","
        "\"tags\":[[\"e\",\"%s\",\"\",\"root\"]]}",
        event_id, root_id);
}

/* Construct a minimal kind:7 reaction JSON referencing an event */
static char *make_reaction_json(const char *event_id, const char *target_id) {
    return g_strdup_printf(
        "{\"id\":\"%s\",\"pubkey\":\"2222222222222222222222222222222222222222222222222222222222222222\","
        "\"kind\":7,\"created_at\":1700000001,\"content\":\"+\","
        "\"tags\":[[\"e\",\"%s\"]]}",
        event_id, target_id);
}

/* Construct a minimal kind:1111 NIP-22 comment JSON */
static char *make_comment_json(const char *event_id, const char *root_id) {
    return g_strdup_printf(
        "{\"id\":\"%s\",\"pubkey\":\"3333333333333333333333333333333333333333333333333333333333333333\","
        "\"kind\":1111,\"created_at\":1700000002,\"content\":\"test comment\","
        "\"tags\":[[\"E\",\"%s\",\"\",\"root\"]]}",
        event_id, root_id);
}

/* Signal counter context */
typedef struct {
    guint reply_count;
    guint reaction_count;
    guint comment_count;
    guint eose_count;
    char *last_reply_json;
    char *last_reaction_json;
    char *last_comment_json;
} SignalCtx;

static void on_reply(GnostrThreadSubscription *sub, const char *json, gpointer data) {
    (void)sub;
    SignalCtx *ctx = data;
    ctx->reply_count++;
    g_free(ctx->last_reply_json);
    ctx->last_reply_json = g_strdup(json);
}

static void on_reaction(GnostrThreadSubscription *sub, const char *json, gpointer data) {
    (void)sub;
    SignalCtx *ctx = data;
    ctx->reaction_count++;
    g_free(ctx->last_reaction_json);
    ctx->last_reaction_json = g_strdup(json);
}

static void on_comment(GnostrThreadSubscription *sub, const char *json, gpointer data) {
    (void)sub;
    SignalCtx *ctx = data;
    ctx->comment_count++;
    g_free(ctx->last_comment_json);
    ctx->last_comment_json = g_strdup(json);
}

static void signal_ctx_clear(SignalCtx *ctx) {
    g_free(ctx->last_reply_json);
    g_free(ctx->last_reaction_json);
    g_free(ctx->last_comment_json);
    memset(ctx, 0, sizeof(*ctx));
}

/* ========== Tests ========== */

static void test_new_and_properties(void) {
    GnostrThreadSubscription *sub = gnostr_thread_subscription_new(ROOT_ID);
    g_assert_nonnull(sub);
    g_assert_cmpstr(gnostr_thread_subscription_get_root_id(sub), ==, ROOT_ID);
    g_assert_false(gnostr_thread_subscription_is_active(sub));
    g_assert_cmpuint(gnostr_thread_subscription_get_seen_count(sub), ==, 0);
    g_object_unref(sub);
}

static void test_start_stop(void) {
    GnostrThreadSubscription *sub = gnostr_thread_subscription_new(ROOT_ID);

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

    g_object_unref(sub);
}

static void test_reply_signal_via_eventbus(void) {
    GnostrThreadSubscription *sub = gnostr_thread_subscription_new(ROOT_ID);
    SignalCtx ctx = {0};

    g_signal_connect(sub, "reply-received", G_CALLBACK(on_reply), &ctx);
    gnostr_thread_subscription_start(sub);

    /* Emit a kind:1 event that references our root */
    char *json = make_reply_json(REPLY_ID, ROOT_ID);
    NostrEventBus *bus = nostr_event_bus_get_default();
    nostr_event_bus_emit(bus, "event::kind::1", json);

    g_assert_cmpuint(ctx.reply_count, ==, 1);
    g_assert_nonnull(ctx.last_reply_json);

    /* Verify deduplication: same event again should not fire */
    nostr_event_bus_emit(bus, "event::kind::1", json);
    g_assert_cmpuint(ctx.reply_count, ==, 1);

    g_assert_cmpuint(gnostr_thread_subscription_get_seen_count(sub), ==, 1);

    g_free(json);
    signal_ctx_clear(&ctx);
    g_object_unref(sub);
}

static void test_reaction_signal_via_eventbus(void) {
    GnostrThreadSubscription *sub = gnostr_thread_subscription_new(ROOT_ID);
    SignalCtx ctx = {0};

    g_signal_connect(sub, "reaction-received", G_CALLBACK(on_reaction), &ctx);
    gnostr_thread_subscription_start(sub);

    char *json = make_reaction_json(REACT_ID, ROOT_ID);
    NostrEventBus *bus = nostr_event_bus_get_default();
    nostr_event_bus_emit(bus, "event::kind::7", json);

    g_assert_cmpuint(ctx.reaction_count, ==, 1);
    g_assert_nonnull(ctx.last_reaction_json);

    g_free(json);
    signal_ctx_clear(&ctx);
    g_object_unref(sub);
}

static void test_comment_signal_via_eventbus(void) {
    GnostrThreadSubscription *sub = gnostr_thread_subscription_new(ROOT_ID);
    SignalCtx ctx = {0};

    g_signal_connect(sub, "comment-received", G_CALLBACK(on_comment), &ctx);
    gnostr_thread_subscription_start(sub);

    /* NIP-22 comment with uppercase E tag */
    char *json = make_comment_json(
        "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
        ROOT_ID);
    NostrEventBus *bus = nostr_event_bus_get_default();
    nostr_event_bus_emit(bus, "event::kind::1111", json);

    g_assert_cmpuint(ctx.comment_count, ==, 1);

    g_free(json);
    signal_ctx_clear(&ctx);
    g_object_unref(sub);
}

static void test_unrelated_event_filtered(void) {
    GnostrThreadSubscription *sub = gnostr_thread_subscription_new(ROOT_ID);
    SignalCtx ctx = {0};

    g_signal_connect(sub, "reply-received", G_CALLBACK(on_reply), &ctx);
    gnostr_thread_subscription_start(sub);

    /* Event referencing a different root - should be filtered */
    char *json = make_reply_json(REPLY_ID, OTHER_ID);
    NostrEventBus *bus = nostr_event_bus_get_default();
    nostr_event_bus_emit(bus, "event::kind::1", json);

    g_assert_cmpuint(ctx.reply_count, ==, 0);

    g_free(json);
    signal_ctx_clear(&ctx);
    g_object_unref(sub);
}

static void test_add_monitored_id(void) {
    GnostrThreadSubscription *sub = gnostr_thread_subscription_new(ROOT_ID);
    SignalCtx ctx = {0};

    g_signal_connect(sub, "reply-received", G_CALLBACK(on_reply), &ctx);
    gnostr_thread_subscription_start(sub);

    /* Event referencing a mid-thread ID (not root) */
    char *mid_id = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    char *json = make_reply_json(REPLY_ID, mid_id);
    NostrEventBus *bus = nostr_event_bus_get_default();

    /* Should be filtered initially */
    nostr_event_bus_emit(bus, "event::kind::1", json);
    g_assert_cmpuint(ctx.reply_count, ==, 0);

    /* Add the mid-thread ID to monitored set */
    gnostr_thread_subscription_add_monitored_id(sub, mid_id);

    /* Now it should match */
    /* Need a different event ID since the first one was already seen in the filter */
    g_free(json);
    json = make_reply_json(
        "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef",
        mid_id);
    nostr_event_bus_emit(bus, "event::kind::1", json);
    g_assert_cmpuint(ctx.reply_count, ==, 1);

    g_free(json);
    signal_ctx_clear(&ctx);
    g_object_unref(sub);
}

static void test_no_signals_after_stop(void) {
    GnostrThreadSubscription *sub = gnostr_thread_subscription_new(ROOT_ID);
    SignalCtx ctx = {0};

    g_signal_connect(sub, "reply-received", G_CALLBACK(on_reply), &ctx);
    gnostr_thread_subscription_start(sub);
    gnostr_thread_subscription_stop(sub);

    /* After stop, events should not trigger signals */
    char *json = make_reply_json(REPLY_ID, ROOT_ID);
    NostrEventBus *bus = nostr_event_bus_get_default();
    nostr_event_bus_emit(bus, "event::kind::1", json);

    g_assert_cmpuint(ctx.reply_count, ==, 0);

    g_free(json);
    signal_ctx_clear(&ctx);
    g_object_unref(sub);
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
