/* nostr-publish-policy.c - Publish policy, OK classification, NIP-65
 *
 * SPDX-License-Identifier: MIT
 *
 * Backoff and OK classification are lifted verbatim from nostr-dav's
 * nd-publisher.c so nostr-dav's outbox behaviour is unchanged.
 */

#include "nostr-publish-policy.h"

#include <json-glib/json-glib.h>

#include <string.h>

#define NP_NIP65_RELAY_LIST_KIND 10002

G_DEFINE_QUARK(nostr-publish-error-quark, nostr_publish_error)

void
nostr_publish_policy_init(NostrPublishPolicy *policy)
{
  g_return_if_fail(policy != NULL);
  memset(policy, 0, sizeof(*policy));
}

gint64
nostr_publish_policy_backoff_delay(const NostrPublishPolicy *policy,
                                   guint                     attempts)
{
  gint64 initial = NOSTR_PUBLISH_DEFAULT_BACKOFF_INITIAL_SEC;
  gint64 max     = NOSTR_PUBLISH_DEFAULT_BACKOFF_MAX_SEC;
  if (policy != NULL && policy->backoff_initial_sec > 0)
    initial = policy->backoff_initial_sec;
  if (policy != NULL && policy->backoff_max_sec > 0)
    max = policy->backoff_max_sec;

  gint64 delay = initial;
  for (guint i = 0; i < attempts && delay < max; i++)
    delay *= 2;
  if (delay > max)
    delay = max;
  return delay;
}

guint
nostr_publish_policy_required_acks(const NostrPublishPolicy *policy,
                                   guint                     n_targets)
{
  if (n_targets == 0)
    return 1;
  guint quorum = policy != NULL ? policy->quorum : 0;
  if (quorum == 0 || quorum > n_targets)
    return n_targets;
  return quorum;
}

/* Appends @url to @out unless already present. */
static void
add_unique(GPtrArray *out, const gchar *url)
{
  for (guint i = 0; i < out->len; i++)
    if (g_str_equal(g_ptr_array_index(out, i), url))
      return;
  g_ptr_array_add(out, g_strdup(url));
}

static GStrv
ptr_array_to_strv(GPtrArray *arr)
{
  g_ptr_array_add(arr, NULL);
  return (GStrv)g_ptr_array_free(arr, FALSE);
}

GStrv
nostr_publish_policy_select_targets(const NostrPublishPolicy *policy,
                                    const gchar *const       *write_relays,
                                    const gchar              *session_relay_url,
                                    GError                  **error)
{
  NostrPublishUpstream mode = policy != NULL
    ? policy->upstream : NOSTR_PUBLISH_UPSTREAM_SESSION_RELAY_OR_DIRECT;
  gboolean have_session = session_relay_url != NULL && *session_relay_url;

  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  switch (mode) {
  case NOSTR_PUBLISH_UPSTREAM_SESSION_RELAY_ONLY:
    if (have_session)
      add_unique(out, session_relay_url);
    break;
  case NOSTR_PUBLISH_UPSTREAM_SESSION_RELAY_OR_DIRECT:
    if (have_session) {
      add_unique(out, session_relay_url);
      break;
    }
    G_GNUC_FALLTHROUGH;
  case NOSTR_PUBLISH_UPSTREAM_DIRECT_ONLY:
    if (write_relays != NULL)
      for (guint i = 0; write_relays[i] != NULL; i++)
        if (*write_relays[i] != '\0')
          add_unique(out, write_relays[i]);
    break;
  }

  if (out->len == 0) {
    g_ptr_array_unref(out);
    g_set_error(error, NOSTR_PUBLISH_ERROR, NOSTR_PUBLISH_ERROR_NO_RELAYS,
                mode == NOSTR_PUBLISH_UPSTREAM_SESSION_RELAY_ONLY
                  ? "upstream policy is session-relay-only but no session "
                    "relay is available"
                  : "no write relays to publish to");
    return NULL;
  }
  return ptr_array_to_strv(out);
}

NostrPublishOkClass
nostr_publish_classify_ok(gboolean accepted, const gchar *reason)
{
  if (accepted)
    return NOSTR_PUBLISH_OK_ACCEPT;
  if (reason == NULL)
    return NOSTR_PUBLISH_OK_TRANSIENT;
  if (g_str_has_prefix(reason, "duplicate:"))
    return NOSTR_PUBLISH_OK_ACCEPT;
  if (g_str_has_prefix(reason, "invalid:") ||
      g_str_has_prefix(reason, "blocked:") ||
      g_str_has_prefix(reason, "banned:") ||
      /* NIP-42 retry-after-AUTH is not implemented; a relay that refuses
       * without a signed AUTH frame is effectively permanent for us.
       * Classifying as transient would spin the retry loop forever. */
      g_str_has_prefix(reason, "restricted:") ||
      g_str_has_prefix(reason, "auth-required:"))
    return NOSTR_PUBLISH_OK_PERMANENT;
  return NOSTR_PUBLISH_OK_TRANSIENT;
}

GStrv
nostr_publish_nip65_relays(const gchar            *relay_list_json,
                           NostrPublishNip65Usage  usage,
                           GError                **error)
{
  g_return_val_if_fail(relay_list_json != NULL, NULL);

  g_autoptr(JsonParser) parser = json_parser_new();
  GError *parse_err = NULL;
  if (!json_parser_load_from_data(parser, relay_list_json, -1, &parse_err)) {
    g_set_error(error, NOSTR_PUBLISH_ERROR,
                NOSTR_PUBLISH_ERROR_INVALID_RELAY_LIST,
                "relay list is not valid JSON: %s", parse_err->message);
    g_clear_error(&parse_err);
    return NULL;
  }
  JsonNode *root = json_parser_get_root(parser);
  if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_set_error_literal(error, NOSTR_PUBLISH_ERROR,
                        NOSTR_PUBLISH_ERROR_INVALID_RELAY_LIST,
                        "relay list is not a JSON object");
    return NULL;
  }
  JsonObject *obj = json_node_get_object(root);
  JsonNode *kind = json_object_get_member(obj, "kind");
  if (kind == NULL || !JSON_NODE_HOLDS_VALUE(kind) ||
      json_node_get_value_type(kind) != G_TYPE_INT64 ||
      json_node_get_int(kind) != NP_NIP65_RELAY_LIST_KIND) {
    g_set_error(error, NOSTR_PUBLISH_ERROR,
                NOSTR_PUBLISH_ERROR_INVALID_RELAY_LIST,
                "relay list is not a kind-%d event",
                NP_NIP65_RELAY_LIST_KIND);
    return NULL;
  }
  JsonNode *tags_node = json_object_get_member(obj, "tags");
  if (tags_node == NULL || !JSON_NODE_HOLDS_ARRAY(tags_node)) {
    g_set_error_literal(error, NOSTR_PUBLISH_ERROR,
                        NOSTR_PUBLISH_ERROR_INVALID_RELAY_LIST,
                        "relay list has no tags array");
    return NULL;
  }

  JsonArray *tags = json_node_get_array(tags_node);
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < json_array_get_length(tags); i++) {
    JsonNode *tn = json_array_get_element(tags, i);
    if (tn == NULL || !JSON_NODE_HOLDS_ARRAY(tn))
      continue;
    JsonArray *tag = json_node_get_array(tn);
    guint len = json_array_get_length(tag);
    if (len < 2)
      continue;
    JsonNode *name = json_array_get_element(tag, 0);
    JsonNode *url  = json_array_get_element(tag, 1);
    if (!JSON_NODE_HOLDS_VALUE(name) || !JSON_NODE_HOLDS_VALUE(url) ||
        json_node_get_value_type(name) != G_TYPE_STRING ||
        json_node_get_value_type(url) != G_TYPE_STRING ||
        !g_str_equal(json_node_get_string(name), "r"))
      continue;

    /* Unmarked = read + write; tags with an unknown marker are skipped. */
    NostrPublishNip65Usage tag_usage =
      NOSTR_PUBLISH_NIP65_READ | NOSTR_PUBLISH_NIP65_WRITE;
    if (len >= 3) {
      JsonNode *marker = json_array_get_element(tag, 2);
      const gchar *m = (JSON_NODE_HOLDS_VALUE(marker) &&
                        json_node_get_value_type(marker) == G_TYPE_STRING)
                         ? json_node_get_string(marker) : NULL;
      if (g_strcmp0(m, "read") == 0)
        tag_usage = NOSTR_PUBLISH_NIP65_READ;
      else if (g_strcmp0(m, "write") == 0)
        tag_usage = NOSTR_PUBLISH_NIP65_WRITE;
      else if (m != NULL && *m != '\0')
        continue;
    }
    if ((tag_usage & usage) == 0)
      continue;

    const gchar *u = json_node_get_string(url);
    if (!g_str_has_prefix(u, "wss://") && !g_str_has_prefix(u, "ws://"))
      continue;
    add_unique(out, u);
  }
  return ptr_array_to_strv(out);
}
