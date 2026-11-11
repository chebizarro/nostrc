#ifndef GH_CALL_PAINTABLE_H
#define GH_CALL_PAINTABLE_H
#include <gtk/gtk.h>
#include <gst/gst.h>
G_BEGIN_DECLS
#define GH_TYPE_CALL_PAINTABLE (gh_call_paintable_get_type())
G_DECLARE_FINAL_TYPE(GhCallPaintable, gh_call_paintable, GH, CALL_PAINTABLE, GObject)
GhCallPaintable *gh_call_paintable_new(GMainContext *context);
/* Full reference, sink consumes RGBA; configure videoconvert upstream. */
GstElement *gh_call_paintable_create_sink(GhCallPaintable *self);
void gh_call_paintable_stop(GhCallPaintable *self);
G_END_DECLS
#endif
