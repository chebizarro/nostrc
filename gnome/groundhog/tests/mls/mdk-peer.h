/* The MDK side of the interop tests (nostrc-7gx7, nostrc-77pa): the JSON-lines
 * driver of tests/interop/mdk/driver (MDK v0.8.0 peers), usually a Docker
 * container. GH_MDK_DRIVER holds its command line (shell-quoted), e.g.
 *   docker run --rm -i --network host nostrc-mdk-interop:0.8.0
 * (CMake sets it when BUILD_MDK_INTEROP is ON). The driver talks to the
 * test's own local relays (mls-world.h), which live on this process's main
 * context: every call iterates it while it waits for the answer, bounded by
 * WAIT_SECONDS as a failure, never as progress. Header-only; include it after
 * mls-world.h. */
#ifndef GH_TEST_MDK_PEER_H
#define GH_TEST_MDK_PEER_H

#include <json-glib/json-glib.h>

typedef struct {
  GSubprocess *process;
  GOutputStream *requests;
  GDataInputStream *answers;
  guint next_id;
  gchar *line;       /* the answer line read, until taken */
  gboolean ended;    /* the driver closed its stdout, or reading failed */
} MdkDriver;

static void
mdk_line_read(GObject *source, GAsyncResult *result, gpointer data)
{
  MdkDriver *driver = data;
  g_autoptr(GError) error = NULL;
  gchar *line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(source), result,
                                                          NULL, &error);
  if (!line) {
    if (error)
      g_printerr("mdk driver: %s\n", error->message);
    driver->ended = TRUE;
    return;
  }
  driver->line = line;
}

static gboolean
mdk_answered(gpointer data)
{
  MdkDriver *driver = data;
  return driver->line != NULL || driver->ended;
}

/* Starts the driver; FALSE (nothing started) when GH_MDK_DRIVER is unset. */
static G_GNUC_UNUSED gboolean
mdk_driver_start(MdkDriver *driver)
{
  memset(driver, 0, sizeof *driver);
  const gchar *command = g_getenv("GH_MDK_DRIVER");
  if (!command || !*command)
    return FALSE;
  g_auto(GStrv) argv = NULL;
  g_autoptr(GError) error = NULL;
  g_assert_true(g_shell_parse_argv(command, NULL, &argv, &error));
  g_assert_no_error(error);
  driver->process = g_subprocess_newv((const gchar *const *)argv,
                                      G_SUBPROCESS_FLAGS_STDIN_PIPE |
                                        G_SUBPROCESS_FLAGS_STDOUT_PIPE,
                                      &error);
  g_assert_no_error(error);
  driver->requests = g_subprocess_get_stdin_pipe(driver->process);
  driver->answers = g_data_input_stream_new(g_subprocess_get_stdout_pipe(driver->process));
  return TRUE;
}

static G_GNUC_UNUSED void
mdk_driver_stop(MdkDriver *driver)
{
  if (!driver->process)
    return;
  g_output_stream_close(driver->requests, NULL, NULL);   /* EOF: the driver exits */
  g_autoptr(GError) error = NULL;
  g_subprocess_wait(driver->process, NULL, &error);
  g_clear_object(&driver->answers);
  g_clear_object(&driver->process);
  g_free(driver->line);
  memset(driver, 0, sizeof *driver);
}

/* Sends {"id":N, body} (body: the request's members, e.g. "\"cmd\":\"hello\"")
 * and returns the answer object (transfer full), whether it is ok or not. */
static G_GNUC_UNUSED JsonObject *
mdk_vcall(MdkDriver *driver, const gchar *format, va_list args)
{
  g_autofree gchar *body = g_strdup_vprintf(format, args);
  guint id = ++driver->next_id;
  g_autofree gchar *request = g_strdup_printf("{\"id\":%u,%s}\n", id, body);
  g_autoptr(GError) error = NULL;
  g_assert_true(g_output_stream_write_all(driver->requests, request, strlen(request), NULL, NULL,
                                          &error));
  g_assert_no_error(error);
  g_assert_true(g_output_stream_flush(driver->requests, NULL, &error));
  g_assert_no_error(error);
  g_data_input_stream_read_line_async(driver->answers, G_PRIORITY_DEFAULT, NULL, mdk_line_read,
                                      driver);
  spin_until(mdk_answered, driver, "the MDK driver's answer");
  g_assert_false(driver->ended);
  g_autofree gchar *line = g_steal_pointer(&driver->line);
  g_autoptr(JsonParser) parser = json_parser_new();
  g_assert_true(json_parser_load_from_data(parser, line, -1, &error));
  g_assert_no_error(error);
  JsonNode *root = json_parser_get_root(parser);
  g_assert_true(JSON_NODE_HOLDS_OBJECT(root));
  JsonObject *answer = json_object_ref(json_node_get_object(root));
  g_assert_cmpint(json_object_get_int_member(answer, "id"), ==, id);
  return answer;
}

/* The answer, which may be a failure (ok false, "error"). */
static G_GNUC_UNUSED G_GNUC_PRINTF(2, 3) JsonObject *
mdk_try(MdkDriver *driver, const gchar *format, ...)
{
  va_list args;
  va_start(args, format);
  JsonObject *answer = mdk_vcall(driver, format, args);
  va_end(args);
  return answer;
}

/* The answer, asserted ok. */
static G_GNUC_UNUSED G_GNUC_PRINTF(2, 3) JsonObject *
mdk_call(MdkDriver *driver, const gchar *format, ...)
{
  va_list args;
  va_start(args, format);
  JsonObject *answer = mdk_vcall(driver, format, args);
  va_end(args);
  if (!json_object_get_boolean_member_with_default(answer, "ok", FALSE))
    g_error("MDK driver refused: %s",
            json_object_get_string_member_with_default(answer, "error", "(no error)"));
  return answer;
}

static G_GNUC_UNUSED gchar *
mdk_json(JsonObject *object)
{
  g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
  json_node_set_object(node, object);
  return json_to_string(node, FALSE);
}

/* Whether a JSON array of strings holds value. */
static G_GNUC_UNUSED gboolean
mdk_has(JsonArray *array, const gchar *value)
{
  for (guint i = 0; array && i < json_array_get_length(array); i++)
    if (g_strcmp0(json_array_get_string_element(array, i), value) == 0)
      return TRUE;
  return FALSE;
}

static int
mdk_strcmp(const void *a, const void *b)
{
  return g_strcmp0(*(const gchar *const *)a, *(const gchar *const *)b);
}

/* A JSON array of strings as a sorted GStrv (transfer full). */
static G_GNUC_UNUSED GStrv
mdk_strv(JsonArray *array)
{
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
  for (guint i = 0; array && i < json_array_get_length(array); i++)
    g_strv_builder_add(builder, json_array_get_string_element(array, i));
  GStrv out = g_strv_builder_end(builder);
  qsort(out, g_strv_length(out), sizeof(gchar *), mdk_strcmp);
  return out;
}

#endif
