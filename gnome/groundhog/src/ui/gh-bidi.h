/* gh-bidi.h — direction-aware text alignment (nostrc-gofet.10).
 *
 * GtkLabel xalign/justify are measured from the widget's start edge: GTK
 * mirrors them when the widget direction is RTL. UI copy inherits the
 * locale's direction, so a fixed xalign of 0 means "start". Text written by
 * people (message bodies, names, poll questions) can have the other
 * direction than the UI: an Arabic message in an English session, or an
 * English one under Arabic. Such labels take their direction from their own
 * first strong character, so they start at their own reading edge, and fall
 * back to the inherited direction when the text has no strong character
 * (numbers, emoji, punctuation).
 */
#pragma once

#include <gtk/gtk.h>

static inline GtkTextDirection
gh_bidi_direction_for_text(const char *text)
{
  /* Deprecated without a replacement for this exact question (the first
   * strong character's direction, UAX #9 P2); GTK itself still relies on it. */
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  PangoDirection dir = text ? pango_find_base_dir(text, -1) : PANGO_DIRECTION_NEUTRAL;
  G_GNUC_END_IGNORE_DEPRECATIONS
  switch (dir) {
  case PANGO_DIRECTION_RTL:
    return GTK_TEXT_DIR_RTL;
  case PANGO_DIRECTION_LTR:
    return GTK_TEXT_DIR_LTR;
  default:
    return GTK_TEXT_DIR_NONE;
  }
}

/* Gives a label showing people's text the direction of that text. Call after
 * setting the label's text or markup; the visible text is inspected, so
 * markup tags and link targets do not decide the direction. */
static inline void
gh_bidi_label_follow_content(GtkLabel *label)
{
  gtk_widget_set_direction(GTK_WIDGET(label),
                           gh_bidi_direction_for_text(gtk_label_get_text(label)));
}
