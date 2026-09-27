/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-key-package-discovery.c - Marmot KeyPackage discovery via kind:10002
 * write relays (nostrc-prqu.11). See the header.
 */
#include "gn-key-package-discovery.h"

#include <json-glib/json-glib.h>
#include <marmot-gobject-1.0/marmot-gobject.h>
#include <nostr-event.h>
#include <string.h>

#define KIND_RELAY_LIST 10002

/* ---- relay list parsing ------------------------------------------------ */

static gboolean
relay_url_ok(const char *url)
{
  return url && (g_str_has_prefix(url, "wss://") || g_str_has_prefix(url, "ws://")) &&
         strlen(url) > 6 && !strpbrk(url, " \t\r\n");
}

static char *
normalize_url(const char *url)
{
  char *u = g_strdup(url);
  gsize n = strlen(u);
  while (n > 6 && u[n - 1] == '/')
    u[--n] = '\0';
  return u;
}

char **
gn_kp_write_relays_from_relay_list(const char *event_json)
{
  GPtrArray *out = g_ptr_array_new();
  g_autoptr(JsonParser) p = json_parser_new();
  JsonObject *ev = NULL;
  if (event_json && json_parser_load_from_data(p, event_json, -1, NULL) &&
      JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p)))
    ev = json_node_get_object(json_parser_get_root(p));

  JsonArray *tags = NULL;
  if (ev && json_object_has_member(ev, "kind") &&
      json_object_get_int_member(ev, "kind") == KIND_RELAY_LIST &&
      json_object_has_member(ev, "tags") &&
      JSON_NODE_HOLDS_ARRAY(json_object_get_member(ev, "tags")))
    tags = json_object_get_array_member(ev, "tags");

  guint n = tags ? json_array_get_length(tags) : 0;
  for (guint i = 0; i < n; i++) {
    JsonNode *tn = json_array_get_element(tags, i);
    if (!JSON_NODE_HOLDS_ARRAY(tn)) continue;
    JsonArray *tag = json_node_get_array(tn);
    guint len = json_array_get_length(tag);
    if (len < 2) continue;
    const char *name = json_array_get_string_element(tag, 0);
    const char *url = json_array_get_string_element(tag, 1);
    const char *marker = len >= 3 ? json_array_get_string_element(tag, 2) : NULL;
    if (g_strcmp0(name, "r") != 0 || !relay_url_ok(url))
      continue;
    /* NIP-65: unmarked = read+write; "read" = read-only. */
    if (marker && *marker && g_strcmp0(marker, "write") != 0)
      continue;
    g_autofree char *norm = normalize_url(url);
    gboolean dup = FALSE;
    for (guint j = 0; j < out->len && !dup; j++)
      dup = g_strcmp0(g_ptr_array_index(out, j), norm) == 0;
    if (!dup)
      g_ptr_array_add(out, g_steal_pointer(&norm));
  }
  g_ptr_array_add(out, NULL);
  return (char **)g_ptr_array_free(out, FALSE);
}

/* Newest kind:10002 of @pubkey_hex among @events that validates. */
static const char *
newest_relay_list(GPtrArray *events, const char *pubkey_hex)
{
  const char *best = NULL;
  gint64 best_at = -1;
  for (guint i = 0; events && i < events->len; i++) {
    const char *json = g_ptr_array_index(events, i);
    NostrEvent *ev = nostr_event_new();
    gboolean ok = ev &&
        nostr_event_deserialize_signed(ev, json, NULL) == NOSTR_EVENT_VALIDATION_OK &&
        nostr_event_validate(ev, NULL) == NOSTR_EVENT_VALIDATION_OK &&
        nostr_event_get_kind(ev) == KIND_RELAY_LIST &&
        g_ascii_strcasecmp(nostr_event_get_pubkey(ev), pubkey_hex) == 0;
    gint64 at = ok ? nostr_event_get_created_at(ev) : -1;
    if (ev) nostr_event_free(ev);
    if (ok && at > best_at) {
      best = json;
      best_at = at;
    }
  }
  return best;
}

static char *
relay_list_filter(const char *pubkey_hex)
{
  return g_strdup_printf("{\"kinds\":[%d],\"authors\":[\"%s\"]}", KIND_RELAY_LIST, pubkey_hex);
}

char **
gn_kp_local_write_relays(const GnKpBackend *backend, const char *pubkey_hex)
{
  g_autofree char *filter = relay_list_filter(pubkey_hex);
  g_autoptr(GPtrArray) local = backend->query_local(backend->data, filter);
  const char *list = newest_relay_list(local, pubkey_hex);
  return gn_kp_write_relays_from_relay_list(list);
}

/* ---- discovery ------------------------------------------------------------ */

typedef struct {
  GnKpBackend backend;
  char *pubkey_hex;
  GPtrArray *candidates;   /* event JSONs */
} Discovery;

static void
discovery_free(Discovery *d)
{
  g_free(d->pubkey_hex);
  g_ptr_array_unref(d->candidates);
  g_free(d);
}

static void
add_all(GPtrArray *into, GPtrArray *events)
{
  for (guint i = 0; events && i < events->len; i++)
    g_ptr_array_add(into, g_strdup(g_ptr_array_index(events, i)));
}

static void
finish_selection(GTask *task)
{
  Discovery *d = g_task_get_task_data(task);
  g_ptr_array_add(d->candidates, NULL);
  GError *error = NULL;
  gint idx = marmot_gobject_select_key_package_event(
      (const gchar *const *)d->candidates->pdata, d->pubkey_hex, &error);
  g_ptr_array_remove_index(d->candidates, d->candidates->len - 1);
  if (idx < 0) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "No usable key package (kind:30443) found for %s: %s",
                            d->pubkey_hex, error ? error->message : "no candidates");
    g_clear_error(&error);
  } else {
    g_task_return_pointer(task, g_strdup(g_ptr_array_index(d->candidates, idx)), g_free);
  }
  g_object_unref(task);
}

static void
on_key_packages(GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void)source;
  GTask *task = user_data;
  Discovery *d = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) events = d->backend.query_relays_finish(d->backend.data, res, &error);
  if (error)
    g_debug("KeyPackageDiscovery: relay query failed: %s", error->message);
  add_all(d->candidates, events);
  finish_selection(task);
}

static char *
key_package_filter(const char *pubkey_hex)
{
  return g_strdup_printf("{\"kinds\":[%d],\"authors\":[\"%s\"]}",
                         MARMOT_GOBJECT_KIND_KEY_PACKAGE, pubkey_hex);
}

static void
fetch_key_packages(GTask *task, char **write_relays /* transfer full */)
{
  Discovery *d = g_task_get_task_data(task);
  g_autofree char *filter = key_package_filter(d->pubkey_hex);

  g_autoptr(GPtrArray) local = d->backend.query_local(d->backend.data, filter);
  add_all(d->candidates, local);

  if (!write_relays || !write_relays[0]) {
    g_debug("KeyPackageDiscovery: %s has no kind:10002 write relays; local store only",
            d->pubkey_hex);
    g_strfreev(write_relays);
    finish_selection(task);
    return;
  }
  d->backend.query_relays_async(d->backend.data, (const char *const *)write_relays, filter,
                                g_task_get_cancellable(task), on_key_packages, task);
  g_strfreev(write_relays);
}

static void
on_relay_list(GObject *source, GAsyncResult *res, gpointer user_data)
{
  (void)source;
  GTask *task = user_data;
  Discovery *d = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) events = d->backend.query_relays_finish(d->backend.data, res, &error);
  const char *list = newest_relay_list(events, d->pubkey_hex);
  fetch_key_packages(task, gn_kp_write_relays_from_relay_list(list));
}

void
gn_kp_discover_async(const GnKpBackend *backend, const char *pubkey_hex,
                     GCancellable *cancellable, GAsyncReadyCallback callback,
                     gpointer user_data)
{
  g_return_if_fail(backend != NULL && pubkey_hex != NULL);
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  g_task_set_source_tag(task, gn_kp_discover_async);
  Discovery *d = g_new0(Discovery, 1);
  d->backend = *backend;
  d->pubkey_hex = g_ascii_strdown(pubkey_hex, -1);
  d->candidates = g_ptr_array_new_with_free_func(g_free);
  g_task_set_task_data(task, d, (GDestroyNotify)discovery_free);

  /* The invitee's relay list: local store first, else the user's relays. */
  char **write = gn_kp_local_write_relays(&d->backend, d->pubkey_hex);
  if (write[0] || !backend->own_relays) {
    fetch_key_packages(task, write);
    return;
  }
  g_strfreev(write);
  g_auto(GStrv) own = backend->own_relays(backend->data);
  if (!own || !own[0]) {
    fetch_key_packages(task, NULL);
    return;
  }
  g_autofree char *filter = relay_list_filter(d->pubkey_hex);
  backend->query_relays_async(backend->data, (const char *const *)own, filter,
                              cancellable, on_relay_list, task);
}

char *
gn_kp_discover_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, NULL), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}
