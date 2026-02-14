/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 gnostr contributors
 *
 * test_thread_graph_model.c - Unit tests for GnostrThreadGraphModel
 *
 * nostrc-pp64 (Epic 4.2): Tests incremental thread graph updates,
 * NIP-10 parsing, parent-child relationships, and signal emission.
 */

#include <glib.h>
#include <nostr-gobject-1.0/gnostr-thread-graph-model.h>

/* Sample IDs */
#define ROOT_ID   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define REPLY1_ID "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define REPLY2_ID "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
#define REACT_ID  "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
#define NESTED_ID "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
#define PUBKEY1   "1111111111111111111111111111111111111111111111111111111111111111"

static char *make_note(const char *id, int kind, const char *root, const char *reply) {
    GString *s = g_string_new(NULL);
    g_string_printf(s,
        "{\"id\":\"%s\",\"pubkey\":\"%s\",\"kind\":%d,"
        "\"created_at\":1700000000,\"content\":\"hello\",\"tags\":[",
        id, PUBKEY1, kind);

    if (root) {
        g_string_append_printf(s, "[\"e\",\"%s\",\"\",\"root\"]", root);
        if (reply) g_string_append_printf(s, ",[\"e\",\"%s\",\"\",\"reply\"]", reply);
    }

    g_string_append(s, "]}");
    return g_string_free(s, FALSE);
}

static char *make_reaction(const char *id, const char *target) {
    return g_strdup_printf(
        "{\"id\":\"%s\",\"pubkey\":\"%s\",\"kind\":7,"
        "\"created_at\":1700000001,\"content\":\"+\","
        "\"tags\":[[\"e\",\"%s\"]]}",
        id, PUBKEY1, target);
}

/* Signal tracking */
typedef struct {
    guint reply_added;
    guint reaction_added;
    guint event_updated;
    char *last_reply_id;
    char *last_reply_parent;
    char *last_reaction_target;
} SigCtx;

static void on_reply_added(GnostrThreadGraphModel *m, const char *id,
                            const char *parent, SigCtx *ctx) {
    (void)m;
    ctx->reply_added++;
    g_free(ctx->last_reply_id);
    ctx->last_reply_id = g_strdup(id);
    g_free(ctx->last_reply_parent);
    ctx->last_reply_parent = g_strdup(parent);
}

static void on_reaction_added(GnostrThreadGraphModel *m, const char *id,
                               const char *target, SigCtx *ctx) {
    (void)m; (void)id;
    ctx->reaction_added++;
    g_free(ctx->last_reaction_target);
    ctx->last_reaction_target = g_strdup(target);
}

static void on_event_updated(GnostrThreadGraphModel *m, const char *id, SigCtx *ctx) {
    (void)m; (void)id;
    ctx->event_updated++;
}

static void sigctx_clear(SigCtx *ctx) {
    g_free(ctx->last_reply_id);
    g_free(ctx->last_reply_parent);
    g_free(ctx->last_reaction_target);
    memset(ctx, 0, sizeof(*ctx));
}

/* ========== Tests ========== */

static void test_new_model(void) {
    GnostrThreadGraphModel *model = gnostr_thread_graph_model_new(ROOT_ID);
    g_assert_nonnull(model);
    g_assert_cmpstr(gnostr_thread_graph_model_get_root_id(model), ==, ROOT_ID);
    g_assert_cmpuint(gnostr_thread_graph_model_get_node_count(model), ==, 0);
    g_assert_cmpuint(gnostr_thread_graph_model_get_reply_count(model), ==, 0);
    g_object_unref(model);
}

static void test_add_root_event(void) {
    GnostrThreadGraphModel *model = gnostr_thread_graph_model_new(ROOT_ID);
    char *json = make_note(ROOT_ID, 1, NULL, NULL);

    gboolean added = gnostr_thread_graph_model_add_event_json(model, json);
    g_assert_true(added);
    g_assert_cmpuint(gnostr_thread_graph_model_get_node_count(model), ==, 1);

    const GnostrThreadGraphNode *node = gnostr_thread_graph_model_get_node(model, ROOT_ID);
    g_assert_nonnull(node);
    g_assert_cmpstr(node->event_id, ==, ROOT_ID);
    g_assert_cmpint(node->kind, ==, 1);
    g_assert_cmpuint(node->depth, ==, 0);

    /* Duplicate is rejected */
    g_assert_false(gnostr_thread_graph_model_add_event_json(model, json));

    g_free(json);
    g_object_unref(model);
}

static void test_reply_links_to_parent(void) {
    GnostrThreadGraphModel *model = gnostr_thread_graph_model_new(ROOT_ID);
    SigCtx ctx = {0};
    g_signal_connect(model, "reply-added", G_CALLBACK(on_reply_added), &ctx);

    /* Add root first */
    char *root_json = make_note(ROOT_ID, 1, NULL, NULL);
    gnostr_thread_graph_model_add_event_json(model, root_json);

    /* Add reply referencing root */
    char *reply_json = make_note(REPLY1_ID, 1, ROOT_ID, NULL);
    gnostr_thread_graph_model_add_event_json(model, reply_json);

    /* Check parent-child link */
    const GnostrThreadGraphNode *root = gnostr_thread_graph_model_get_node(model, ROOT_ID);
    g_assert_cmpuint(root->child_ids->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(root->child_ids, 0), ==, REPLY1_ID);

    const GnostrThreadGraphNode *reply = gnostr_thread_graph_model_get_node(model, REPLY1_ID);
    g_assert_cmpuint(reply->depth, ==, 1);
    g_assert_cmpstr(reply->parent_id, ==, ROOT_ID);

    /* Check signal was emitted */
    g_assert_cmpuint(ctx.reply_added, ==, 2); /* root + reply */
    g_assert_cmpstr(ctx.last_reply_id, ==, REPLY1_ID);
    g_assert_cmpstr(ctx.last_reply_parent, ==, ROOT_ID);

    g_free(root_json);
    g_free(reply_json);
    sigctx_clear(&ctx);
    g_object_unref(model);
}

static void test_nested_reply_depth(void) {
    GnostrThreadGraphModel *model = gnostr_thread_graph_model_new(ROOT_ID);

    char *root_json = make_note(ROOT_ID, 1, NULL, NULL);
    char *r1_json = make_note(REPLY1_ID, 1, ROOT_ID, NULL);
    /* Nested reply: root=ROOT_ID, reply=REPLY1_ID */
    char *nested_json = make_note(NESTED_ID, 1, ROOT_ID, REPLY1_ID);

    gnostr_thread_graph_model_add_event_json(model, root_json);
    gnostr_thread_graph_model_add_event_json(model, r1_json);
    gnostr_thread_graph_model_add_event_json(model, nested_json);

    const GnostrThreadGraphNode *nested = gnostr_thread_graph_model_get_node(model, NESTED_ID);
    g_assert_cmpuint(nested->depth, ==, 2);
    g_assert_cmpstr(nested->parent_id, ==, REPLY1_ID);

    /* REPLY1 should have NESTED as child */
    const GnostrThreadGraphNode *r1 = gnostr_thread_graph_model_get_node(model, REPLY1_ID);
    g_assert_cmpuint(r1->child_ids->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(r1->child_ids, 0), ==, NESTED_ID);

    g_free(root_json);
    g_free(r1_json);
    g_free(nested_json);
    g_object_unref(model);
}

static void test_reaction_increments_count(void) {
    GnostrThreadGraphModel *model = gnostr_thread_graph_model_new(ROOT_ID);
    SigCtx ctx = {0};
    g_signal_connect(model, "reaction-added", G_CALLBACK(on_reaction_added), &ctx);
    g_signal_connect(model, "event-updated", G_CALLBACK(on_event_updated), &ctx);

    char *root_json = make_note(ROOT_ID, 1, NULL, NULL);
    gnostr_thread_graph_model_add_event_json(model, root_json);

    char *react_json = make_reaction(REACT_ID, ROOT_ID);
    gnostr_thread_graph_model_add_event_json(model, react_json);

    const GnostrThreadGraphNode *root = gnostr_thread_graph_model_get_node(model, ROOT_ID);
    g_assert_cmpuint(root->reaction_count, ==, 1);

    g_assert_cmpuint(ctx.reaction_added, ==, 1);
    g_assert_cmpstr(ctx.last_reaction_target, ==, ROOT_ID);
    g_assert_cmpuint(ctx.event_updated, ==, 1);

    g_free(root_json);
    g_free(react_json);
    sigctx_clear(&ctx);
    g_object_unref(model);
}

static void test_orphan_relinks_when_parent_arrives(void) {
    GnostrThreadGraphModel *model = gnostr_thread_graph_model_new(ROOT_ID);

    /* Add child before parent (out-of-order arrival) */
    char *reply_json = make_note(REPLY1_ID, 1, ROOT_ID, NULL);
    gnostr_thread_graph_model_add_event_json(model, reply_json);

    /* Reply should be orphaned (depth 1 as it references root but root not present) */
    const GnostrThreadGraphNode *reply = gnostr_thread_graph_model_get_node(model, REPLY1_ID);
    g_assert_nonnull(reply);

    /* Now add root */
    char *root_json = make_note(ROOT_ID, 1, NULL, NULL);
    gnostr_thread_graph_model_add_event_json(model, root_json);

    /* Root should now list reply as child */
    const GnostrThreadGraphNode *root = gnostr_thread_graph_model_get_node(model, ROOT_ID);
    g_assert_cmpuint(root->child_ids->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(root->child_ids, 0), ==, REPLY1_ID);

    /* Reply depth should be recalculated */
    reply = gnostr_thread_graph_model_get_node(model, REPLY1_ID);
    g_assert_cmpuint(reply->depth, ==, 1);

    g_free(root_json);
    g_free(reply_json);
    g_object_unref(model);
}

static void test_render_order(void) {
    GnostrThreadGraphModel *model = gnostr_thread_graph_model_new(ROOT_ID);

    char *root_json = make_note(ROOT_ID, 1, NULL, NULL);
    char *r1_json = make_note(REPLY1_ID, 1, ROOT_ID, NULL);
    char *r2_json = make_note(REPLY2_ID, 1, ROOT_ID, NULL);

    gnostr_thread_graph_model_add_event_json(model, root_json);
    gnostr_thread_graph_model_add_event_json(model, r1_json);
    gnostr_thread_graph_model_add_event_json(model, r2_json);

    GPtrArray *order = gnostr_thread_graph_model_get_render_order(model);
    g_assert_nonnull(order);
    g_assert_cmpuint(order->len, ==, 3);

    /* Root should be first */
    g_assert_cmpstr(g_ptr_array_index(order, 0), ==, ROOT_ID);

    g_ptr_array_unref(order);
    g_free(root_json);
    g_free(r1_json);
    g_free(r2_json);
    g_object_unref(model);
}

static void test_clear(void) {
    GnostrThreadGraphModel *model = gnostr_thread_graph_model_new(ROOT_ID);

    char *json = make_note(ROOT_ID, 1, NULL, NULL);
    gnostr_thread_graph_model_add_event_json(model, json);
    g_assert_cmpuint(gnostr_thread_graph_model_get_node_count(model), ==, 1);

    gnostr_thread_graph_model_clear(model);
    g_assert_cmpuint(gnostr_thread_graph_model_get_node_count(model), ==, 0);
    g_assert_cmpuint(gnostr_thread_graph_model_get_reply_count(model), ==, 0);

    g_free(json);
    g_object_unref(model);
}

int main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/thread-graph-model/new", test_new_model);
    g_test_add_func("/thread-graph-model/add-root", test_add_root_event);
    g_test_add_func("/thread-graph-model/reply-links", test_reply_links_to_parent);
    g_test_add_func("/thread-graph-model/nested-depth", test_nested_reply_depth);
    g_test_add_func("/thread-graph-model/reaction-count", test_reaction_increments_count);
    g_test_add_func("/thread-graph-model/orphan-relink", test_orphan_relinks_when_parent_arrives);
    g_test_add_func("/thread-graph-model/render-order", test_render_order);
    g_test_add_func("/thread-graph-model/clear", test_clear);

    return g_test_run();
}
