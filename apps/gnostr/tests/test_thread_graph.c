/**
 * Thread Graph Unit Tests
 *
 * Tests for the thread view graph building logic.
 * Tests: single note, linear thread, branching, deep nesting,
 * disconnected nodes, missing parent handling, focus path calculation.
 */

#include <glib.h>
#include <string.h>

/* We need the ThreadGraph types but not the full GnostrThreadView.
 * Define minimal stubs for GTK types used in the header. */
typedef struct _GtkWidget GtkWidget;
typedef struct _GnostrThreadView GnostrThreadView;

/* Define the types we're testing (copied from gnostr-thread-view.c) */

typedef struct {
  char *id_hex;
  char *pubkey_hex;
  char *content;
  char *root_id;
  char *parent_id;
  char *root_relay_hint;
  char *parent_relay_hint;
  GPtrArray *mentioned_pubkeys;
  gint64 created_at;
  guint depth;
  char *display_name;
  char *handle;
  char *avatar_url;
  char *nip05;
} ThreadEventItem;

typedef struct _ThreadNode {
  ThreadEventItem *event;
  GPtrArray *child_ids;
  char *parent_id;
  guint depth;
  gboolean is_focus_path;
  gboolean is_collapsed;
  guint child_count;
} ThreadNode;

typedef struct _ThreadGraph {
  GHashTable *nodes;
  char *root_id;
  char *focus_id;
  GPtrArray *render_order;
} ThreadGraph;

/* Helper functions for testing */

static void thread_event_item_free(ThreadEventItem *item) {
  if (!item) return;
  g_free(item->id_hex);
  g_free(item->pubkey_hex);
  g_free(item->content);
  g_free(item->root_id);
  g_free(item->parent_id);
  g_free(item->root_relay_hint);
  g_free(item->parent_relay_hint);
  if (item->mentioned_pubkeys) g_ptr_array_unref(item->mentioned_pubkeys);
  g_free(item->display_name);
  g_free(item->handle);
  g_free(item->avatar_url);
  g_free(item->nip05);
  g_free(item);
}

static void thread_node_free(ThreadNode *node) {
  if (!node) return;
  if (node->child_ids) g_ptr_array_unref(node->child_ids);
  g_free(node->parent_id);
  g_free(node);
}

static ThreadNode *thread_node_new(ThreadEventItem *event) {
  ThreadNode *node = g_new0(ThreadNode, 1);
  node->event = event;
  node->child_ids = g_ptr_array_new_with_free_func(g_free);
  node->parent_id = event->parent_id ? g_strdup(event->parent_id) : NULL;
  node->depth = 0;
  node->is_focus_path = FALSE;
  node->is_collapsed = FALSE;
  node->child_count = 0;
  return node;
}

static void thread_graph_free(ThreadGraph *graph) {
  if (!graph) return;
  if (graph->nodes) g_hash_table_unref(graph->nodes);
  if (graph->render_order) g_ptr_array_unref(graph->render_order);
  g_free(graph->root_id);
  g_free(graph->focus_id);
  g_free(graph);
}

static ThreadGraph *thread_graph_new(void) {
  ThreadGraph *graph = g_new0(ThreadGraph, 1);
  graph->nodes = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                        (GDestroyNotify)thread_node_free);
  graph->render_order = g_ptr_array_new();
  return graph;
}

/* Create a mock event for testing */
static ThreadEventItem *create_mock_event(const char *id,
                                           const char *parent_id,
                                           const char *root_id,
                                           gint64 created_at) {
  ThreadEventItem *item = g_new0(ThreadEventItem, 1);
  item->id_hex = g_strdup(id);
  item->parent_id = parent_id ? g_strdup(parent_id) : NULL;
  item->root_id = root_id ? g_strdup(root_id) : NULL;
  item->created_at = created_at;
  item->pubkey_hex = g_strdup("0000000000000000000000000000000000000000000000000000000000000001");
  item->content = g_strdup("test content");
  return item;
}

/* Test helper: build graph from events hash table */
static ThreadGraph *build_test_graph(GHashTable *events_by_id,
                                      const char *focus_id,
                                      const char *root_id) {
  if (g_hash_table_size(events_by_id) == 0) return NULL;

  ThreadGraph *graph = thread_graph_new();
  graph->focus_id = focus_id ? g_strdup(focus_id) : NULL;
  graph->root_id = root_id ? g_strdup(root_id) : NULL;

  /* Step 1: Create nodes for all events */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, events_by_id);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    ThreadEventItem *item = (ThreadEventItem *)value;
    ThreadNode *node = thread_node_new(item);
    g_hash_table_insert(graph->nodes, item->id_hex, node);
  }

  /* Step 2: Build parent->children relationships */
  g_hash_table_iter_init(&iter, graph->nodes);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    ThreadNode *node = (ThreadNode *)value;
    if (!node->event) continue;

    const char *parent_id = node->event->parent_id;
    if (parent_id && strlen(parent_id) == 64) {
      ThreadNode *parent_node = g_hash_table_lookup(graph->nodes, parent_id);
      if (parent_node) {
        g_ptr_array_add(parent_node->child_ids, g_strdup(node->event->id_hex));
      }
    }
  }

  /* Step 3: Find root node (no parent in our set) */
  const char *discovered_root = NULL;
  g_hash_table_iter_init(&iter, graph->nodes);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    ThreadNode *node = (ThreadNode *)value;
    if (!node->event) continue;

    const char *parent_id = node->event->parent_id;
    if (!parent_id || !g_hash_table_contains(graph->nodes, parent_id)) {
      if (graph->root_id &&
          g_strcmp0(node->event->id_hex, graph->root_id) == 0) {
        discovered_root = node->event->id_hex;
        break;
      }
      if (!discovered_root) {
        discovered_root = node->event->id_hex;
      } else {
        ThreadNode *current_root = g_hash_table_lookup(graph->nodes, discovered_root);
        if (current_root && current_root->event &&
            node->event->created_at < current_root->event->created_at) {
          discovered_root = node->event->id_hex;
        }
      }
    }
  }

  if (discovered_root && !graph->root_id) {
    graph->root_id = g_strdup(discovered_root);
  }

  /* Step 4: Calculate depths using BFS from root */
  if (graph->root_id) {
    GQueue *queue = g_queue_new();
    ThreadNode *root_node = g_hash_table_lookup(graph->nodes, graph->root_id);
    if (root_node) {
      root_node->depth = 0;
      g_queue_push_tail(queue, root_node);

      while (!g_queue_is_empty(queue)) {
        ThreadNode *node = g_queue_pop_head(queue);
        if (!node->child_ids) continue;

        for (guint i = 0; i < node->child_ids->len; i++) {
          const char *child_id = g_ptr_array_index(node->child_ids, i);
          ThreadNode *child_node = g_hash_table_lookup(graph->nodes, child_id);
          if (child_node) {
            child_node->depth = node->depth + 1;
            g_queue_push_tail(queue, child_node);
          }
        }
      }
      g_queue_free(queue);
    }
  }

  /* Step 5: Mark focus path */
  if (graph->focus_id) {
    const char *current_id = graph->focus_id;
    while (current_id) {
      ThreadNode *node = g_hash_table_lookup(graph->nodes, current_id);
      if (!node) break;
      node->is_focus_path = TRUE;
      current_id = node->parent_id;
    }
  }

  return graph;
}

/* ---- Test Cases ---- */

/* Test: Single note thread (note is its own root) */
static void test_single_note(void) {
  GHashTable *events = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              NULL, (GDestroyNotify)thread_event_item_free);

  /* 64-char hex ID for a single note */
  const char *id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  ThreadEventItem *event = create_mock_event(id, NULL, NULL, 1000);
  g_hash_table_insert(events, event->id_hex, event);

  ThreadGraph *graph = build_test_graph(events, id, NULL);

  g_assert_nonnull(graph);
  g_assert_cmpuint(g_hash_table_size(graph->nodes), ==, 1);
  g_assert_cmpstr(graph->root_id, ==, id);

  ThreadNode *node = g_hash_table_lookup(graph->nodes, id);
  g_assert_nonnull(node);
  g_assert_cmpuint(node->depth, ==, 0);
  g_assert_true(node->is_focus_path);
  g_assert_null(node->parent_id);
  g_assert_cmpuint(node->child_ids->len, ==, 0);

  thread_graph_free(graph);
  g_hash_table_unref(events);
}

/* Test: Linear thread (A -> B -> C) */
static void test_linear_thread(void) {
  GHashTable *events = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              NULL, (GDestroyNotify)thread_event_item_free);

  const char *id_a = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  const char *id_b = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  const char *id_c = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

  ThreadEventItem *a = create_mock_event(id_a, NULL, NULL, 1000);
  ThreadEventItem *b = create_mock_event(id_b, id_a, id_a, 2000);
  ThreadEventItem *c = create_mock_event(id_c, id_b, id_a, 3000);

  g_hash_table_insert(events, a->id_hex, a);
  g_hash_table_insert(events, b->id_hex, b);
  g_hash_table_insert(events, c->id_hex, c);

  /* Focus on C, should mark path C -> B -> A */
  ThreadGraph *graph = build_test_graph(events, id_c, NULL);

  g_assert_nonnull(graph);
  g_assert_cmpuint(g_hash_table_size(graph->nodes), ==, 3);
  g_assert_cmpstr(graph->root_id, ==, id_a);

  /* Check depths */
  ThreadNode *node_a = g_hash_table_lookup(graph->nodes, id_a);
  ThreadNode *node_b = g_hash_table_lookup(graph->nodes, id_b);
  ThreadNode *node_c = g_hash_table_lookup(graph->nodes, id_c);

  g_assert_cmpuint(node_a->depth, ==, 0);
  g_assert_cmpuint(node_b->depth, ==, 1);
  g_assert_cmpuint(node_c->depth, ==, 2);

  /* Check parent-child relationships */
  g_assert_cmpuint(node_a->child_ids->len, ==, 1);
  g_assert_cmpuint(node_b->child_ids->len, ==, 1);
  g_assert_cmpuint(node_c->child_ids->len, ==, 0);

  /* Check focus path - all nodes should be on path */
  g_assert_true(node_a->is_focus_path);
  g_assert_true(node_b->is_focus_path);
  g_assert_true(node_c->is_focus_path);

  thread_graph_free(graph);
  g_hash_table_unref(events);
}

/* Test: Branching thread (A -> B, A -> C) */
static void test_branching_thread(void) {
  GHashTable *events = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              NULL, (GDestroyNotify)thread_event_item_free);

  const char *id_a = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  const char *id_b = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  const char *id_c = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

  ThreadEventItem *a = create_mock_event(id_a, NULL, NULL, 1000);
  ThreadEventItem *b = create_mock_event(id_b, id_a, id_a, 2000);
  ThreadEventItem *c = create_mock_event(id_c, id_a, id_a, 2500);

  g_hash_table_insert(events, a->id_hex, a);
  g_hash_table_insert(events, b->id_hex, b);
  g_hash_table_insert(events, c->id_hex, c);

  /* Focus on B */
  ThreadGraph *graph = build_test_graph(events, id_b, NULL);

  g_assert_nonnull(graph);
  g_assert_cmpuint(g_hash_table_size(graph->nodes), ==, 3);
  g_assert_cmpstr(graph->root_id, ==, id_a);

  ThreadNode *node_a = g_hash_table_lookup(graph->nodes, id_a);
  ThreadNode *node_b = g_hash_table_lookup(graph->nodes, id_b);
  ThreadNode *node_c = g_hash_table_lookup(graph->nodes, id_c);

  /* A should have 2 children */
  g_assert_cmpuint(node_a->child_ids->len, ==, 2);

  /* B and C are siblings at depth 1 */
  g_assert_cmpuint(node_b->depth, ==, 1);
  g_assert_cmpuint(node_c->depth, ==, 1);

  /* Focus path: A and B, but not C */
  g_assert_true(node_a->is_focus_path);
  g_assert_true(node_b->is_focus_path);
  g_assert_false(node_c->is_focus_path);

  thread_graph_free(graph);
  g_hash_table_unref(events);
}

/* Test: Deep nesting (chain of 5 replies) */
static void test_deep_nesting(void) {
  GHashTable *events = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              NULL, (GDestroyNotify)thread_event_item_free);

  const char *ids[5] = {
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
    "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
    "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
  };

  for (int i = 0; i < 5; i++) {
    const char *parent = (i == 0) ? NULL : ids[i - 1];
    const char *root = (i == 0) ? NULL : ids[0];
    ThreadEventItem *e = create_mock_event(ids[i], parent, root, 1000 + i * 1000);
    g_hash_table_insert(events, e->id_hex, e);
  }

  /* Focus on deepest node */
  ThreadGraph *graph = build_test_graph(events, ids[4], NULL);

  g_assert_nonnull(graph);
  g_assert_cmpuint(g_hash_table_size(graph->nodes), ==, 5);
  g_assert_cmpstr(graph->root_id, ==, ids[0]);

  /* Check depths */
  for (int i = 0; i < 5; i++) {
    ThreadNode *node = g_hash_table_lookup(graph->nodes, ids[i]);
    g_assert_cmpuint(node->depth, ==, (guint)i);
    g_assert_true(node->is_focus_path);
  }

  thread_graph_free(graph);
  g_hash_table_unref(events);
}

/* Test: Disconnected nodes (multiple roots) */
static void test_disconnected_nodes(void) {
  GHashTable *events = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              NULL, (GDestroyNotify)thread_event_item_free);

  const char *id_a = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  const char *id_b = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  const char *id_c = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

  /* Three unrelated notes (all roots) */
  ThreadEventItem *a = create_mock_event(id_a, NULL, NULL, 1000);
  ThreadEventItem *b = create_mock_event(id_b, NULL, NULL, 2000);
  ThreadEventItem *c = create_mock_event(id_c, NULL, NULL, 3000);

  g_hash_table_insert(events, a->id_hex, a);
  g_hash_table_insert(events, b->id_hex, b);
  g_hash_table_insert(events, c->id_hex, c);

  ThreadGraph *graph = build_test_graph(events, id_b, NULL);

  g_assert_nonnull(graph);
  g_assert_cmpuint(g_hash_table_size(graph->nodes), ==, 3);

  /* Earliest event should be discovered as root */
  g_assert_cmpstr(graph->root_id, ==, id_a);

  /* All nodes are roots (depth 0, no parent in graph) */
  ThreadNode *node_a = g_hash_table_lookup(graph->nodes, id_a);
  ThreadNode *node_b = g_hash_table_lookup(graph->nodes, id_b);
  ThreadNode *node_c = g_hash_table_lookup(graph->nodes, id_c);

  g_assert_cmpuint(node_a->depth, ==, 0);
  g_assert_cmpuint(node_b->depth, ==, 0);
  g_assert_cmpuint(node_c->depth, ==, 0);

  /* Only B is on focus path */
  g_assert_false(node_a->is_focus_path);
  g_assert_true(node_b->is_focus_path);
  g_assert_false(node_c->is_focus_path);

  thread_graph_free(graph);
  g_hash_table_unref(events);
}

/* Test: Missing parent handling */
static void test_missing_parent(void) {
  GHashTable *events = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              NULL, (GDestroyNotify)thread_event_item_free);

  const char *id_a = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  const char *id_b = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  const char *missing = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";

  /* A is root, B references a missing parent */
  ThreadEventItem *a = create_mock_event(id_a, NULL, NULL, 1000);
  ThreadEventItem *b = create_mock_event(id_b, missing, id_a, 2000);

  g_hash_table_insert(events, a->id_hex, a);
  g_hash_table_insert(events, b->id_hex, b);

  ThreadGraph *graph = build_test_graph(events, id_b, NULL);

  g_assert_nonnull(graph);
  g_assert_cmpuint(g_hash_table_size(graph->nodes), ==, 2);

  /* A should still be the root (earliest) */
  g_assert_cmpstr(graph->root_id, ==, id_a);

  ThreadNode *node_a = g_hash_table_lookup(graph->nodes, id_a);
  ThreadNode *node_b = g_hash_table_lookup(graph->nodes, id_b);

  /* A has no children (B's parent is missing, not A) */
  g_assert_cmpuint(node_a->child_ids->len, ==, 0);

  /* B is an orphan (depth 0, parent not in graph) */
  g_assert_cmpuint(node_b->depth, ==, 0);
  g_assert_cmpstr(node_b->parent_id, ==, missing);

  /* Focus path only includes B (can't trace to A) */
  g_assert_false(node_a->is_focus_path);
  g_assert_true(node_b->is_focus_path);

  thread_graph_free(graph);
  g_hash_table_unref(events);
}

/* Test: Focus path calculation with explicit root */
static void test_focus_path_explicit_root(void) {
  GHashTable *events = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              NULL, (GDestroyNotify)thread_event_item_free);

  const char *id_a = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  const char *id_b = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  const char *id_c = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
  const char *id_d = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";

  /* A -> B -> C, A -> D (branch) */
  ThreadEventItem *a = create_mock_event(id_a, NULL, NULL, 1000);
  ThreadEventItem *b = create_mock_event(id_b, id_a, id_a, 2000);
  ThreadEventItem *c = create_mock_event(id_c, id_b, id_a, 3000);
  ThreadEventItem *d = create_mock_event(id_d, id_a, id_a, 2500);

  g_hash_table_insert(events, a->id_hex, a);
  g_hash_table_insert(events, b->id_hex, b);
  g_hash_table_insert(events, c->id_hex, c);
  g_hash_table_insert(events, d->id_hex, d);

  /* Focus on C with explicit root A */
  ThreadGraph *graph = build_test_graph(events, id_c, id_a);

  g_assert_nonnull(graph);
  g_assert_cmpstr(graph->root_id, ==, id_a);

  ThreadNode *node_a = g_hash_table_lookup(graph->nodes, id_a);
  ThreadNode *node_b = g_hash_table_lookup(graph->nodes, id_b);
  ThreadNode *node_c = g_hash_table_lookup(graph->nodes, id_c);
  ThreadNode *node_d = g_hash_table_lookup(graph->nodes, id_d);

  /* Focus path: C -> B -> A */
  g_assert_true(node_a->is_focus_path);
  g_assert_true(node_b->is_focus_path);
  g_assert_true(node_c->is_focus_path);
  g_assert_false(node_d->is_focus_path);

  thread_graph_free(graph);
  g_hash_table_unref(events);
}

/* Test: Empty graph */
static void test_empty_graph(void) {
  GHashTable *events = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              NULL, (GDestroyNotify)thread_event_item_free);

  ThreadGraph *graph = build_test_graph(events, NULL, NULL);

  g_assert_null(graph);

  g_hash_table_unref(events);
}

int main(int argc, char *argv[]) {
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/thread_graph/single_note", test_single_note);
  g_test_add_func("/thread_graph/linear_thread", test_linear_thread);
  g_test_add_func("/thread_graph/branching_thread", test_branching_thread);
  g_test_add_func("/thread_graph/deep_nesting", test_deep_nesting);
  g_test_add_func("/thread_graph/disconnected_nodes", test_disconnected_nodes);
  g_test_add_func("/thread_graph/missing_parent", test_missing_parent);
  g_test_add_func("/thread_graph/focus_path_explicit_root", test_focus_path_explicit_root);
  g_test_add_func("/thread_graph/empty_graph", test_empty_graph);

  return g_test_run();
}
