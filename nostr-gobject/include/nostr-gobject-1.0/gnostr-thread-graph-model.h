/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 gnostr contributors
 *
 * gnostr-thread-graph-model.h - Incremental thread graph with reactive updates
 *
 * nostrc-pp64 (Epic 4.2): Maintains a thread graph with parent-child
 * relationships. Supports incremental additions from GnostrThreadSubscription
 * events without full rebuild. Emits granular signals for UI updates.
 */

#ifndef GNOSTR_THREAD_GRAPH_MODEL_H
#define GNOSTR_THREAD_GRAPH_MODEL_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_THREAD_GRAPH_MODEL (gnostr_thread_graph_model_get_type())
G_DECLARE_FINAL_TYPE(GnostrThreadGraphModel, gnostr_thread_graph_model,
                     GNOSTR, THREAD_GRAPH_MODEL, GObject)

/**
 * GnostrThreadGraphNode:
 * @event_id: hex event ID (owned)
 * @pubkey: hex pubkey of author (owned)
 * @content: event content text (owned)
 * @created_at: UNIX timestamp
 * @kind: event kind (1, 7, 1111, etc.)
 * @root_id: NIP-10 root reference (nullable, owned)
 * @parent_id: NIP-10 reply/parent reference (nullable, owned)
 * @depth: distance from root in the graph
 * @child_ids: (element-type utf8): array of child event IDs
 * @reaction_count: number of kind:7 reactions on this event
 *
 * A node in the thread graph. Contains parsed event data and
 * graph relationships for bidirectional traversal.
 */
typedef struct {
    char *event_id;
    char *pubkey;
    char *content;
    gint64 created_at;
    gint kind;
    char *root_id;
    char *parent_id;
    guint depth;
    GPtrArray *child_ids;     /* (element-type utf8) owned strings */
    guint reaction_count;
} GnostrThreadGraphNode;

/**
 * gnostr_thread_graph_model_new:
 * @root_event_id: the 64-character hex event ID of the thread root
 *
 * Creates a new empty thread graph model for the given root.
 *
 * Returns: (transfer full): a new #GnostrThreadGraphModel
 */
GnostrThreadGraphModel *gnostr_thread_graph_model_new(const char *root_event_id);

/**
 * gnostr_thread_graph_model_add_event_json:
 * @self: a #GnostrThreadGraphModel
 * @event_json: the event as a JSON string
 *
 * Parses the JSON event and adds it to the graph incrementally.
 * Establishes parent-child links based on NIP-10 tags.
 * Emits "reply-added" or "reaction-added" signal as appropriate.
 *
 * Returns: %TRUE if the event was new and added, %FALSE if duplicate or error
 */
gboolean gnostr_thread_graph_model_add_event_json(GnostrThreadGraphModel *self,
                                                    const char *event_json);

/**
 * gnostr_thread_graph_model_get_node:
 * @self: a #GnostrThreadGraphModel
 * @event_id: the 64-character hex event ID
 *
 * Returns: (transfer none) (nullable): the graph node, or %NULL if not found
 */
const GnostrThreadGraphNode *gnostr_thread_graph_model_get_node(
    GnostrThreadGraphModel *self, const char *event_id);

/**
 * gnostr_thread_graph_model_get_root_id:
 * @self: a #GnostrThreadGraphModel
 *
 * Returns: (transfer none): the root event ID
 */
const char *gnostr_thread_graph_model_get_root_id(GnostrThreadGraphModel *self);

/**
 * gnostr_thread_graph_model_get_node_count:
 * @self: a #GnostrThreadGraphModel
 *
 * Returns: the total number of nodes in the graph
 */
guint gnostr_thread_graph_model_get_node_count(GnostrThreadGraphModel *self);

/**
 * gnostr_thread_graph_model_get_reply_count:
 * @self: a #GnostrThreadGraphModel
 *
 * Returns: the number of kind:1 reply nodes
 */
guint gnostr_thread_graph_model_get_reply_count(GnostrThreadGraphModel *self);

/**
 * gnostr_thread_graph_model_get_children:
 * @self: a #GnostrThreadGraphModel
 * @event_id: the parent event ID
 *
 * Returns: (transfer none) (nullable): GPtrArray of child event ID strings
 */
GPtrArray *gnostr_thread_graph_model_get_children(GnostrThreadGraphModel *self,
                                                   const char *event_id);

/**
 * gnostr_thread_graph_model_get_render_order:
 * @self: a #GnostrThreadGraphModel
 *
 * Returns a flat array of event IDs in depth-first tree traversal order,
 * suitable for rendering the thread as an indented list.
 *
 * Returns: (transfer full) (element-type utf8): GPtrArray of event ID strings.
 *          Free with g_ptr_array_unref().
 */
GPtrArray *gnostr_thread_graph_model_get_render_order(GnostrThreadGraphModel *self);

/**
 * gnostr_thread_graph_model_clear:
 * @self: a #GnostrThreadGraphModel
 *
 * Removes all nodes from the graph.
 */
void gnostr_thread_graph_model_clear(GnostrThreadGraphModel *self);

/**
 * Signals:
 * - "reply-added" (const char *event_id, const char *parent_id) -
 *   A new kind:1 or kind:1111 reply was added to the graph.
 *   parent_id may be NULL if the reply references the root directly.
 *
 * - "reaction-added" (const char *event_id, const char *target_id) -
 *   A new kind:7 reaction was added. target_id is the event being reacted to.
 *
 * - "event-updated" (const char *event_id) -
 *   An existing event was updated (e.g., child count changed).
 */

G_END_DECLS

#endif /* GNOSTR_THREAD_GRAPH_MODEL_H */
