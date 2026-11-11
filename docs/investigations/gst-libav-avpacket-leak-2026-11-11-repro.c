/* Standalone gst-libav AVPacket leak reproducer (nostrc-mazyq); no Groundhog
 * code and no sanitizer suppressions. Name the sink "out" to count frames.
 *
 * cc -g -fsanitize=address -fno-omit-frame-pointer repro.c \
 *   $(pkg-config --cflags --libs gstreamer-1.0) -o repro
 * ASAN_OPTIONS=detect_leaks=1:fast_unwind_on_malloc=0 ./repro \
 *   'videotestsrc num-buffers=300 ! video/x-raw,width=640,height=480 ! openh264enc ! h264parse ! avdec_h264 ! fakesink name=out'
 */
#include <gst/gst.h>

static void count_frame(GstElement *sink, GstBuffer *buffer, GstPad *pad, gpointer data)
{
  (void)sink; (void)buffer; (void)pad;
  g_atomic_int_inc((gint *)data);
}

int main(int argc, char **argv)
{
  gst_init(&argc, &argv);
  if (argc != 2) { gst_deinit(); return 2; }
  GError *error = NULL;
  GstElement *pipeline = gst_parse_launch(argv[1], &error);
  if (!pipeline || error) {
    g_printerr("Parse failed: %s\n", error ? error->message : "no pipeline");
    g_clear_error(&error);
    if (pipeline) gst_object_unref(pipeline);
    gst_deinit(); return 2;
  }
  gint frames = 0;
  GstElement *out = gst_bin_get_by_name(GST_BIN(pipeline), "out");
  if (out) {
    g_object_set(out, "signal-handoffs", TRUE, NULL);
    g_signal_connect(out, "handoff", G_CALLBACK(count_frame), &frames);
    gst_object_unref(out);
  }
  GstBus *bus = gst_element_get_bus(pipeline);
  GstMessage *message = NULL;
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE)
    message = gst_bus_timed_pop_filtered(bus, 60 * GST_SECOND,
                                        GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  int result = !message || GST_MESSAGE_TYPE(message) != GST_MESSAGE_EOS;
  if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
    gst_message_parse_error(message, &error, NULL);
    g_printerr("Pipeline failed: %s\n", error->message);
    g_clear_error(&error);
  }
  if (message) gst_message_unref(message);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  g_print("eos=%s frames=%d\n", result ? "no" : "yes", g_atomic_int_get(&frames));
  gst_object_unref(bus);
  gst_object_unref(pipeline);
  gst_deinit();
  return result;
}
