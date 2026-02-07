#pragma once

#include <glib.h>

/**
 * GnostrDebounce:
 *
 * A reusable debounce timer. When triggered, it cancels any pending
 * invocation and schedules the callback after the configured interval.
 * Guarantees proper cleanup on disposal and only fires on the main thread.
 *
 * Usage:
 *   self->debounce = gnostr_debounce_new(300, on_search_changed, self);
 *   gnostr_debounce_trigger(self->debounce);   // restarts timer
 *   gnostr_debounce_cancel(self->debounce);     // cancel without firing
 *   gnostr_debounce_free(self->debounce);       // cleanup (cancels if pending)
 */
typedef struct _GnostrDebounce GnostrDebounce;

/**
 * gnostr_debounce_new:
 * @interval_ms: debounce interval in milliseconds
 * @callback: function to call when the timer fires (returns %G_SOURCE_REMOVE)
 * @user_data: data passed to @callback
 *
 * Creates a new debounce timer. The callback signature matches #GSourceFunc.
 *
 * Returns: (transfer full): a new #GnostrDebounce, free with gnostr_debounce_free()
 */
GnostrDebounce *gnostr_debounce_new(guint interval_ms,
                                     GSourceFunc callback,
                                     gpointer user_data);

/**
 * gnostr_debounce_trigger:
 * @debounce: a #GnostrDebounce
 *
 * (Re)starts the debounce timer. If a timer is already pending, it is
 * cancelled and a new one is scheduled. The callback will fire after
 * the configured interval with no further triggers.
 */
void gnostr_debounce_trigger(GnostrDebounce *debounce);

/**
 * gnostr_debounce_cancel:
 * @debounce: a #GnostrDebounce
 *
 * Cancels any pending invocation without firing the callback.
 */
void gnostr_debounce_cancel(GnostrDebounce *debounce);

/**
 * gnostr_debounce_is_pending:
 * @debounce: a #GnostrDebounce
 *
 * Returns: %TRUE if a timer is currently pending
 */
gboolean gnostr_debounce_is_pending(GnostrDebounce *debounce);

/**
 * gnostr_debounce_free:
 * @debounce: (nullable): a #GnostrDebounce
 *
 * Cancels any pending invocation and frees the debounce timer.
 * Safe to call with %NULL.
 */
void gnostr_debounce_free(GnostrDebounce *debounce);
