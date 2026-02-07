#include "debounce.h"

struct _GnostrDebounce {
    guint source_id;
    guint interval_ms;
    GSourceFunc callback;
    gpointer user_data;
};

GnostrDebounce *
gnostr_debounce_new(guint interval_ms, GSourceFunc callback, gpointer user_data)
{
    g_return_val_if_fail(callback != NULL, NULL);
    g_return_val_if_fail(interval_ms > 0, NULL);

    GnostrDebounce *d = g_new0(GnostrDebounce, 1);
    d->interval_ms = interval_ms;
    d->callback = callback;
    d->user_data = user_data;
    return d;
}

void
gnostr_debounce_trigger(GnostrDebounce *debounce)
{
    g_return_if_fail(debounce != NULL);

    if (debounce->source_id > 0) {
        g_source_remove(debounce->source_id);
        debounce->source_id = 0;
    }

    debounce->source_id = g_timeout_add(debounce->interval_ms,
                                         debounce->callback,
                                         debounce->user_data);
}

void
gnostr_debounce_cancel(GnostrDebounce *debounce)
{
    g_return_if_fail(debounce != NULL);

    if (debounce->source_id > 0) {
        g_source_remove(debounce->source_id);
        debounce->source_id = 0;
    }
}

gboolean
gnostr_debounce_is_pending(GnostrDebounce *debounce)
{
    g_return_val_if_fail(debounce != NULL, FALSE);
    return debounce->source_id > 0;
}

void
gnostr_debounce_free(GnostrDebounce *debounce)
{
    if (!debounce) return;
    gnostr_debounce_cancel(debounce);
    g_free(debounce);
}
