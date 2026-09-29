/* Window activation in GUI tests (nostrc-9g6e). Whether a window is active
 * is the desktop's to say, and the desktop is shared: under a parallel run
 * another test process presents a window and takes activation (the key
 * window on macOS, the input focus on a shared Xvfb) at any moment, and may
 * give it back. What the code under test does only while its window is
 * active (announcements, following the shown conversation) is therefore
 * asserted against the activation it saw: a GhTestActiveSpan watches
 * GtkWindow:is-active over the turns in which the code decides. Unchanged
 * over the span, the decision is known exactly; changed, either outcome is
 * right, and the test says which it accepts. Header-only. */
#ifndef GH_TEST_ACTIVE_H
#define GH_TEST_ACTIVE_H

#include <gtk/gtk.h>

typedef struct {
  GtkWindow *window;
  gulong handler;
  gboolean active; /* at the start of the span */
  guint changes;   /* is-active notifications during it */
} GhTestActiveSpan;

static G_GNUC_UNUSED void
gh_test_active_span_changed(GObject *window, GParamSpec *pspec, gpointer data)
{
  (void)window;
  (void)pspec;
  ((GhTestActiveSpan *)data)->changes++;
}

/* Starts watching before the code under test gets a turn. */
static G_GNUC_UNUSED void
gh_test_active_span_begin(GhTestActiveSpan *span, GtkWindow *window)
{
  span->window = window;
  span->active = gtk_window_is_active(window);
  span->changes = 0;
  span->handler = g_signal_connect(window, "notify::is-active",
                                   G_CALLBACK(gh_test_active_span_changed), span);
}

/* Ends the span. TRUE when the window's activation did not change during
 * it; then *active (optional) is that activation. */
static G_GNUC_UNUSED gboolean
gh_test_active_span_end(GhTestActiveSpan *span, gboolean *active)
{
  g_clear_signal_handler(&span->handler, span->window);
  gboolean steady = span->changes == 0 && gtk_window_is_active(span->window) == span->active;
  if (active)
    *active = span->active;
  return steady;
}

/* How many of @events (each counted only while the window was active) the
 * code under test must have counted over the span: all or none when the
 * activation held; when it changed, *any is set and any count up to @events
 * is right. */
static G_GNUC_UNUSED guint
gh_test_active_span_expect(GhTestActiveSpan *span, guint events, gboolean *any)
{
  gboolean active = FALSE;
  *any = !gh_test_active_span_end(span, &active);
  return active ? events : 0;
}

#endif
