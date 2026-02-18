/* note-card-binding-ctx.h — Ref-counted async context for NoteCardRow binding cycles.
 *
 * Each bind cycle gets its own NoteCardBindingContext. Async callbacks capture
 * a ref to the context, not a raw pointer to the row. When the context is
 * cancelled (during unbind), callbacks bail out safely — no dangling pointers,
 * no racing with the re-bind disposed=FALSE reset.
 *
 * SPDX-License-Identifier: MIT
 * nostrc-ncr-lifecycle: NoteCardRow lifecycle hardening
 */

#ifndef NOTE_CARD_BINDING_CTX_H
#define NOTE_CARD_BINDING_CTX_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#ifndef NOTE_CARD_BINDING_CTX_TYPEDEF
#define NOTE_CARD_BINDING_CTX_TYPEDEF
typedef struct _NoteCardBindingContext NoteCardBindingContext;
#endif

/**
 * note_card_binding_context_new:
 * @row: The NoteCardRow being bound (a weak reference is stored)
 *
 * Creates a new binding context for a single bind cycle.
 * The returned context has a ref count of 1 (caller owns).
 *
 * Returns: (transfer full): A new #NoteCardBindingContext
 */
NoteCardBindingContext *note_card_binding_context_new (GObject *row);

/**
 * note_card_binding_context_ref:
 * @ctx: A #NoteCardBindingContext
 *
 * Atomically increments the reference count.
 *
 * Returns: (transfer full): @ctx with an incremented ref count
 */
NoteCardBindingContext *note_card_binding_context_ref (NoteCardBindingContext *ctx);

/**
 * note_card_binding_context_unref:
 * @ctx: (transfer full): A #NoteCardBindingContext
 *
 * Atomically decrements the reference count. When it reaches zero,
 * the context is freed (weak ref cleared, cancellable released).
 */
void note_card_binding_context_unref (NoteCardBindingContext *ctx);

/**
 * note_card_binding_context_cancel:
 * @ctx: A #NoteCardBindingContext
 *
 * Marks this context as cancelled and fires the internal GCancellable.
 * Once cancelled, note_card_binding_context_get_row() always returns NULL.
 * This is idempotent — calling it multiple times is safe.
 *
 * Called from prepare_for_unbind() and quiesce().
 */
void note_card_binding_context_cancel (NoteCardBindingContext *ctx);

/**
 * note_card_binding_context_is_cancelled:
 * @ctx: A #NoteCardBindingContext
 *
 * Returns: TRUE if this context has been cancelled.
 */
gboolean note_card_binding_context_is_cancelled (NoteCardBindingContext *ctx);

/**
 * note_card_binding_context_get_row:
 * @ctx: A #NoteCardBindingContext
 *
 * Safely retrieves the owning NoteCardRow, or NULL if:
 * - The context was cancelled (stale callback from a previous binding)
 * - The row widget was finalized (GWeakRef returns NULL)
 *
 * If non-NULL, the returned object has an incremented reference count.
 * The caller MUST call g_object_unref() when done.
 *
 * Returns: (transfer full) (nullable): The row, or NULL if stale/finalized
 */
GObject *note_card_binding_context_get_row (NoteCardBindingContext *ctx);

/**
 * note_card_binding_context_get_cancellable:
 * @ctx: A #NoteCardBindingContext
 *
 * Returns the GCancellable for this binding cycle. Use this instead of
 * per-operation cancellables to automatically cancel all async work when
 * the binding cycle ends.
 *
 * Returns: (transfer none): The context's #GCancellable
 */
GCancellable *note_card_binding_context_get_cancellable (NoteCardBindingContext *ctx);

/**
 * note_card_binding_context_get_binding_id:
 * @ctx: A #NoteCardBindingContext
 *
 * Returns the monotonically-increasing binding ID for this cycle.
 *
 * Returns: The binding ID
 */
guint64 note_card_binding_context_get_binding_id (NoteCardBindingContext *ctx);

G_END_DECLS

#endif /* NOTE_CARD_BINDING_CTX_H */
