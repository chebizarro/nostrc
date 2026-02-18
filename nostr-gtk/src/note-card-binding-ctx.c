/* note-card-binding-ctx.c — Implementation of ref-counted binding context.
 *
 * SPDX-License-Identifier: MIT
 * nostrc-ncr-lifecycle: NoteCardRow lifecycle hardening
 */

#include "note-card-binding-ctx.h"

/* Global monotonic counter for binding IDs.
 * Overflow at 2^64 is not a practical concern (~584 years at 1 billion binds/sec). */
static guint64 s_binding_id_counter = 0;

struct _NoteCardBindingContext {
  gatomicrefcount ref_count;

  /* Weak reference to the owning NoteCardRow.
   * Returns NULL via g_weak_ref_get() after the row is finalized. */
  GWeakRef row_ref;

  /* Unique ID for this binding cycle */
  guint64 binding_id;

  /* One-directional cancellation flag — set in cancel(), never unset.
   * This is the key difference from the old `disposed` boolean which was
   * reset during re-bind, creating a race with in-flight callbacks. */
  gboolean cancelled;

  /* GCancellable for this binding cycle's async I/O operations.
   * Cancelled in note_card_binding_context_cancel(). */
  GCancellable *cancellable;
};

NoteCardBindingContext *
note_card_binding_context_new (GObject *row)
{
  g_return_val_if_fail (G_IS_OBJECT (row), NULL);

  NoteCardBindingContext *ctx = g_new0 (NoteCardBindingContext, 1);
  g_atomic_ref_count_init (&ctx->ref_count);
  g_weak_ref_init (&ctx->row_ref, row);
  ctx->binding_id = ++s_binding_id_counter;
  ctx->cancelled = FALSE;
  ctx->cancellable = g_cancellable_new ();

  return ctx;
}

NoteCardBindingContext *
note_card_binding_context_ref (NoteCardBindingContext *ctx)
{
  g_return_val_if_fail (ctx != NULL, NULL);
  g_atomic_ref_count_inc (&ctx->ref_count);
  return ctx;
}

static void
note_card_binding_context_free (NoteCardBindingContext *ctx)
{
  g_weak_ref_clear (&ctx->row_ref);
  g_clear_object (&ctx->cancellable);
  g_free (ctx);
}

void
note_card_binding_context_unref (NoteCardBindingContext *ctx)
{
  g_return_if_fail (ctx != NULL);
  if (g_atomic_ref_count_dec (&ctx->ref_count))
    note_card_binding_context_free (ctx);
}

void
note_card_binding_context_cancel (NoteCardBindingContext *ctx)
{
  g_return_if_fail (ctx != NULL);

  /* Idempotent — safe to call multiple times (quiesce + dispose + unbind) */
  if (ctx->cancelled)
    return;

  ctx->cancelled = TRUE;

  if (ctx->cancellable)
    g_cancellable_cancel (ctx->cancellable);
}

gboolean
note_card_binding_context_is_cancelled (NoteCardBindingContext *ctx)
{
  g_return_val_if_fail (ctx != NULL, TRUE);
  return ctx->cancelled;
}

GObject *
note_card_binding_context_get_row (NoteCardBindingContext *ctx)
{
  g_return_val_if_fail (ctx != NULL, NULL);

  /* Fast path: if cancelled, don't even try the weak ref.
   * This is the primary guard against the recycling race. */
  if (ctx->cancelled)
    return NULL;

  /* g_weak_ref_get() returns NULL if the object was finalized,
   * or a strong ref if it's still alive. */
  GObject *row = g_weak_ref_get (&ctx->row_ref);
  if (!row)
    return NULL;

  /* Double-check: if cancellation happened between our check above and
   * the weak_ref_get, release the ref and return NULL. */
  if (ctx->cancelled) {
    g_object_unref (row);
    return NULL;
  }

  return row; /* caller owns this ref */
}

GCancellable *
note_card_binding_context_get_cancellable (NoteCardBindingContext *ctx)
{
  g_return_val_if_fail (ctx != NULL, NULL);
  return ctx->cancellable;
}

guint64
note_card_binding_context_get_binding_id (NoteCardBindingContext *ctx)
{
  g_return_val_if_fail (ctx != NULL, 0);
  return ctx->binding_id;
}
