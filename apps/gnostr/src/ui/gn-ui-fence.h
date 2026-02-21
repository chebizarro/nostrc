/* gn-ui-fence.h - Reusable UI lifetime fencing for async callbacks
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of gnostr.
 *
 * Copyright (C) 2026 gnostr contributors
 *
 * gnostr is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <gio/gio.h>
#include <glib-object.h>

G_BEGIN_DECLS

/**
 * GnUiFence:
 * @gen: Generation counter, incremented on each lifecycle transition
 * @cancel: Cancellable for current operation set
 *
 * Reusable UI lifetime token for async callback validation.
 *
 * THE PROBLEM:
 * Async callbacks (HTTP, DB queries, image decode, etc.) can complete after
 * their target widget has been disposed, recycled, or rebound to different data.
 * Touching widgets in this state causes:
 * - Segfaults in GTK (gtk_widget_queue_resize on freed memory)
 * - Heap corruption (malloc_consolidate, invalid free)
 * - GLib assertions (pollfd, GObject type checks)
 * - Profile cache corruption (stale pointers)
 *
 * THE SOLUTION:
 * Generation fencing - each UI object has a generation counter that increments
 * on lifecycle transitions (bind/unbind/dispose). Async callbacks capture the
 * generation at creation time and validate it before touching any UI.
 *
 * USAGE PATTERN:
 *
 * 1. Add GnUiFence to your UI object:
 *    struct _MyWidget {
 *      GtkWidget parent;
 *      GnUiFence fence;
 *      ...
 *    };
 *
 * 2. Initialize in init:
 *    gn_ui_fence_init(&self->fence);
 *
 * 3. Bump on lifecycle transitions:
 *    // In bind/unbind/dispose/set_content:
 *    gn_ui_fence_bump(&self->fence);
 *
 * 4. Create async context with generation snapshot:
 *    MyAsyncCtx *ctx = g_new0(MyAsyncCtx, 1);
 *    g_weak_ref_init(&ctx->widget_ref, self);
 *    ctx->generation = gn_ui_fence_gen(&self->fence);
 *    ctx->cancel = gn_ui_fence_cancel_ref(&self->fence);
 *
 * 5. Validate in callback before touching UI:
 *    MyWidget *self = g_weak_ref_get(&ctx->widget_ref);
 *    if (!self) goto out;  // Widget gone
 *
 *    if (ctx->generation != gn_ui_fence_gen(&self->fence)) {
 *      g_debug("Stale callback dropped: gen=%lu current=%lu",
 *              ctx->generation, gn_ui_fence_gen(&self->fence));
 *      g_object_unref(self);
 *      goto out;  // Widget recycled/rebound
 *    }
 *
 *    if (g_cancellable_is_cancelled(ctx->cancel)) {
 *      g_object_unref(self);
 *      goto out;  // Operation cancelled
 *    }
 *
 *    // Safe to update UI
 *    ...
 *    g_object_unref(self);
 *
 * WHEN TO BUMP:
 * - Widget bind/unbind (for recycled list items)
 * - Widget dispose
 * - Content change (set_note_id, set_profile, etc.)
 * - Any transition that invalidates in-flight async work
 *
 * WHY THIS WORKS:
 * - Weak refs prevent use-after-free (NULL if widget destroyed)
 * - Generation check prevents use-after-recycle (stale if widget reused)
 * - Cancellable provides early-exit hint (though callbacks may still fire)
 * - Together: no async callback ever touches UI unless it proves ownership
 */
typedef struct {
    guint64 gen;
    GCancellable *cancel;
} GnUiFence;

/**
 * gn_ui_fence_init:
 * @fence: Fence to initialize
 *
 * Initialize a UI fence. Call this in your object's init function.
 */
static inline void
gn_ui_fence_init(GnUiFence *fence) {
    g_return_if_fail(fence != NULL);
    fence->gen = 1;
    fence->cancel = NULL;
}

/**
 * gn_ui_fence_bump:
 * @fence: Fence to bump
 *
 * Increment generation and cancel all pending operations.
 * Call this on bind/unbind/dispose or any lifecycle transition
 * that invalidates in-flight async work.
 *
 * This is the "hydra sword" - one call invalidates all async callbacks.
 */
static inline void
gn_ui_fence_bump(GnUiFence *fence) {
    g_return_if_fail(fence != NULL);

    fence->gen++;

    if (fence->cancel) {
	g_cancellable_cancel(fence->cancel);
	g_clear_object(&fence->cancel);
    }

    fence->cancel = g_cancellable_new();
}

/**
 * gn_ui_fence_gen:
 * @fence: Fence to query
 *
 * Get current generation counter.
 * Async contexts capture this value at creation time.
 *
 * Returns: Current generation number
 */
static inline guint64
gn_ui_fence_gen(const GnUiFence *fence) {
    g_return_val_if_fail(fence != NULL, 0);
    return fence->gen;
}

/**
 * gn_ui_fence_cancel_ref:
 * @fence: a #GnUiFence
 *
 * Get a new reference to the fence's cancellable for use in async operations.
 * The caller must unref when done.
 *
 * Returns: (transfer full): A new reference to the cancellable, or NULL
 */
static inline GCancellable *
gn_ui_fence_cancel_ref(GnUiFence *fence) {
    g_return_val_if_fail(fence != NULL, NULL);
    if (fence->cancel)
	return g_object_ref(fence->cancel);
    return NULL;
}

/**
 * gn_ui_fence_clear:
 * @fence: a #GnUiFence
 *
 * Clear the fence's cancellable and cancel any pending operations.
 * Safe to call in dispose. Idempotent.
 * Clear the fence, cancelling any pending operations.
 * Call this in your object's dispose/finalize.
 */
static inline void
gn_ui_fence_clear(GnUiFence *fence) {
    g_return_if_fail(fence != NULL);

    if (fence->cancel) {
	g_cancellable_cancel(fence->cancel);
	g_clear_object(&fence->cancel);
    }

    fence->gen = 0;
}

G_END_DECLS
