/**
 * NIP-88: Poll Events Utility Implementation
 */

#include "nip88_polls.h"
#include <json-glib/json-glib.h>
#include <string.h>

gboolean gnostr_nip88_is_poll_kind(int kind) {
  return kind == NIP88_KIND_POLL;
}

gboolean gnostr_nip88_is_response_kind(int kind) {
  return kind == NIP88_KIND_RESPONSE;
}

static void poll_option_free(gpointer p) {
  GnostrNip88PollOption *opt = (GnostrNip88PollOption *)p;
  if (!opt) return;
  g_free(opt->text);
  g_free(opt);
}

GnostrNip88Poll *gnostr_nip88_poll_parse(const char *json_str) {
  if (!json_str || !*json_str) return NULL;

  g_autoptr(JsonParser) parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, json_str, -1, &error)) {
    g_warning("NIP-88: Failed to parse poll JSON: %s", error ? error->message : "unknown");
    if (error) g_error_free(error);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    return NULL;
  }

  JsonObject *obj = json_node_get_object(root);

  /* Check kind */
  if (!json_object_has_member(obj, "kind")) {
    return NULL;
  }
  gint64 kind = json_object_get_int_member(obj, "kind");
  if (kind != NIP88_KIND_POLL) {
    return NULL;
  }

  GnostrNip88Poll *poll = g_new0(GnostrNip88Poll, 1);
  poll->options = g_ptr_array_new_with_free_func(poll_option_free);
  poll->value_maximum = 1; /* Default to single choice */

  /* Get event ID */
  if (json_object_has_member(obj, "id")) {
    poll->event_id = g_strdup(json_object_get_string_member(obj, "id"));
  }

  /* Get pubkey */
  if (json_object_has_member(obj, "pubkey")) {
    poll->pubkey = g_strdup(json_object_get_string_member(obj, "pubkey"));
  }

  /* Get question from content */
  if (json_object_has_member(obj, "content")) {
    poll->question = g_strdup(json_object_get_string_member(obj, "content"));
  }

  /* Get created_at */
  if (json_object_has_member(obj, "created_at")) {
    poll->created_at = json_object_get_int_member(obj, "created_at");
  }

  /* Parse tags */
  if (json_object_has_member(obj, "tags")) {
    JsonArray *tags = json_object_get_array_member(obj, "tags");
    guint n_tags = json_array_get_length(tags);

    for (guint i = 0; i < n_tags; i++) {
      JsonNode *tag_node = json_array_get_element(tags, i);
      if (!JSON_NODE_HOLDS_ARRAY(tag_node)) continue;

      JsonArray *tag = json_node_get_array(tag_node);
      guint tag_len = json_array_get_length(tag);
      if (tag_len < 2) continue;

      const char *tag_name = json_array_get_string_element(tag, 0);
      if (!tag_name) continue;

      if (g_strcmp0(tag_name, "poll_option") == 0 && tag_len >= 3) {
        /* Poll option: ["poll_option", "index", "text"] */
        const char *index_str = json_array_get_string_element(tag, 1);
        const char *text = json_array_get_string_element(tag, 2);

        if (index_str && text) {
          GnostrNip88PollOption *opt = g_new0(GnostrNip88PollOption, 1);
          opt->index = atoi(index_str);
          opt->text = g_strdup(text);
          g_ptr_array_add(poll->options, opt);
        }
      } else if (g_strcmp0(tag_name, "closed_at") == 0) {
        /* Closing time: ["closed_at", "timestamp"] */
        const char *ts_str = json_array_get_string_element(tag, 1);
        if (ts_str) {
          poll->closed_at = g_ascii_strtoll(ts_str, NULL, 10);
        }
      } else if (g_strcmp0(tag_name, "value_maximum") == 0) {
        /* Max selections: ["value_maximum", "count"] */
        const char *max_str = json_array_get_string_element(tag, 1);
        if (max_str) {
          poll->value_maximum = atoi(max_str);
          if (poll->value_maximum < 1) poll->value_maximum = 1;
        }
      }
    }
  }


  /* Validate: must have at least 2 options */
  if (poll->options->len < 2) {
    gnostr_nip88_poll_free(poll);
    return NULL;
  }

  return poll;
}

GnostrNip88Response *gnostr_nip88_response_parse(const char *json_str) {
  if (!json_str || !*json_str) return NULL;

  g_autoptr(JsonParser) parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, json_str, -1, &error)) {
    g_warning("NIP-88: Failed to parse response JSON: %s", error ? error->message : "unknown");
    if (error) g_error_free(error);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    return NULL;
  }

  JsonObject *obj = json_node_get_object(root);

  /* Check kind */
  if (!json_object_has_member(obj, "kind")) {
    return NULL;
  }
  gint64 kind = json_object_get_int_member(obj, "kind");
  if (kind != NIP88_KIND_RESPONSE) {
    return NULL;
  }

  GnostrNip88Response *response = g_new0(GnostrNip88Response, 1);
  response->selected_indices = g_array_new(FALSE, FALSE, sizeof(int));

  /* Get event ID */
  if (json_object_has_member(obj, "id")) {
    response->event_id = g_strdup(json_object_get_string_member(obj, "id"));
  }

  /* Get responder pubkey */
  if (json_object_has_member(obj, "pubkey")) {
    response->responder_pubkey = g_strdup(json_object_get_string_member(obj, "pubkey"));
  }

  /* Get created_at */
  if (json_object_has_member(obj, "created_at")) {
    response->created_at = json_object_get_int_member(obj, "created_at");
  }

  /* Parse tags */
  if (json_object_has_member(obj, "tags")) {
    JsonArray *tags = json_object_get_array_member(obj, "tags");
    guint n_tags = json_array_get_length(tags);

    for (guint i = 0; i < n_tags; i++) {
      JsonNode *tag_node = json_array_get_element(tags, i);
      if (!JSON_NODE_HOLDS_ARRAY(tag_node)) continue;

      JsonArray *tag = json_node_get_array(tag_node);
      guint tag_len = json_array_get_length(tag);
      if (tag_len < 2) continue;

      const char *tag_name = json_array_get_string_element(tag, 0);
      if (!tag_name) continue;

      if (g_strcmp0(tag_name, "e") == 0) {
        /* Event reference: ["e", "poll_id", "", "root"] */
        const char *poll_id = json_array_get_string_element(tag, 1);
        if (poll_id && !response->poll_id) {
          response->poll_id = g_strdup(poll_id);
        }
      } else if (g_strcmp0(tag_name, "response") == 0) {
        /* Selected option: ["response", "index"] */
        const char *index_str = json_array_get_string_element(tag, 1);
        if (index_str) {
          int idx = atoi(index_str);
          g_array_append_val(response->selected_indices, idx);
        }
      }
    }
  }


  /* Validate: must have poll_id and at least one selection */
  if (!response->poll_id || response->selected_indices->len == 0) {
    gnostr_nip88_response_free(response);
    return NULL;
  }

  return response;
}

gboolean gnostr_nip88_poll_is_open(GnostrNip88Poll *poll) {
  if (!poll) return FALSE;
  if (poll->closed_at <= 0) return TRUE; /* No closing time = always open */

  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  return now < poll->closed_at;
}

gboolean gnostr_nip88_poll_is_multiple_choice(GnostrNip88Poll *poll) {
  if (!poll) return FALSE;
  return poll->value_maximum != 1;
}

char *gnostr_nip88_build_poll_tags(GPtrArray *options,
                                    gint64 closed_at,
                                    gboolean multiple_choice) {
  if (!options || options->len < 2) return NULL;

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_array(builder);

  /* Add poll_option tags */
  for (guint i = 0; i < options->len; i++) {
    const char *text = g_ptr_array_index(options, i);
    if (!text || !*text) continue;

    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "poll_option");
    char *idx_str = g_strdup_printf("%u", i);
    json_builder_add_string_value(builder, idx_str);
    g_free(idx_str);
    json_builder_add_string_value(builder, text);
    json_builder_end_array(builder);
  }

  /* Add closed_at if specified */
  if (closed_at > 0) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "closed_at");
    char *ts_str = g_strdup_printf("%" G_GINT64_FORMAT, closed_at);
    json_builder_add_string_value(builder, ts_str);
    g_free(ts_str);
    json_builder_end_array(builder);
  }

  /* Add value_maximum for single choice (default is multiple) */
  if (!multiple_choice) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "value_maximum");
    json_builder_add_string_value(builder, "1");
    json_builder_end_array(builder);
  }

  json_builder_end_array(builder);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(gen, root);
  char *result = json_generator_to_data(gen, NULL);


  return result;
}

char *gnostr_nip88_build_response_tags(const char *poll_id,
                                        const char *poll_pubkey,
                                        int *selected_indices,
                                        gsize num_indices) {
  if (!poll_id || !selected_indices || num_indices == 0) return NULL;

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_array(builder);

  /* Add event reference to poll */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "e");
  json_builder_add_string_value(builder, poll_id);
  json_builder_add_string_value(builder, ""); /* relay hint */
  json_builder_add_string_value(builder, "root");
  json_builder_end_array(builder);

  /* Add pubkey reference to poll author */
  if (poll_pubkey && *poll_pubkey) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "p");
    json_builder_add_string_value(builder, poll_pubkey);
    json_builder_end_array(builder);
  }

  /* Add response tags for each selected option */
  for (gsize i = 0; i < num_indices; i++) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "response");
    char *idx_str = g_strdup_printf("%d", selected_indices[i]);
    json_builder_add_string_value(builder, idx_str);
    g_free(idx_str);
    json_builder_end_array(builder);
  }

  json_builder_end_array(builder);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(gen, root);
  char *result = json_generator_to_data(gen, NULL);


  return result;
}

void gnostr_nip88_poll_free(GnostrNip88Poll *poll) {
  if (!poll) return;
  g_free(poll->event_id);
  g_free(poll->pubkey);
  g_free(poll->question);
  if (poll->options) {
    g_ptr_array_free(poll->options, TRUE);
  }
  g_free(poll);
}

void gnostr_nip88_response_free(GnostrNip88Response *response) {
  if (!response) return;
  g_free(response->event_id);
  g_free(response->poll_id);
  g_free(response->responder_pubkey);
  if (response->selected_indices) {
    g_array_unref(response->selected_indices);
  }
  g_free(response);
}

/* Voter info for tally tracking */
typedef struct {
  GnostrNip88Response *first_response;  /* First (valid) response from voter */
} VoterInfo;

static void voter_info_free(gpointer p) {
  VoterInfo *info = (VoterInfo *)p;
  if (info) {
    /* Note: we don't own the response, just reference it */
    g_free(info);
  }
}

GnostrNip88VoteTally *gnostr_nip88_tally_votes(GPtrArray *responses,
                                                 gsize num_options) {
  if (!responses || num_options == 0) return NULL;

  GnostrNip88VoteTally *tally = g_new0(GnostrNip88VoteTally, 1);
  tally->num_options = num_options;
  tally->vote_counts = g_new0(guint, num_options);
  tally->total_voters = 0;
  tally->voter_map = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, voter_info_free);

  /* Process responses, keeping only first response per voter */
  for (guint i = 0; i < responses->len; i++) {
    GnostrNip88Response *resp = g_ptr_array_index(responses, i);
    if (!resp || !resp->responder_pubkey) continue;

    /* Check if this voter already voted */
    if (g_hash_table_contains(tally->voter_map, resp->responder_pubkey)) {
      /* Already voted - keep first vote only (by event creation time) */
      VoterInfo *existing = g_hash_table_lookup(tally->voter_map, resp->responder_pubkey);
      if (existing && existing->first_response) {
        /* If new response is earlier, use it instead */
        if (resp->created_at < existing->first_response->created_at) {
          /* Remove old vote counts */
          for (guint j = 0; j < existing->first_response->selected_indices->len; j++) {
            int idx = g_array_index(existing->first_response->selected_indices, int, j);
            if (idx >= 0 && (gsize)idx < num_options && tally->vote_counts[idx] > 0) {
              tally->vote_counts[idx]--;
            }
          }
          /* Add new vote counts */
          for (guint j = 0; j < resp->selected_indices->len; j++) {
            int idx = g_array_index(resp->selected_indices, int, j);
            if (idx >= 0 && (gsize)idx < num_options) {
              tally->vote_counts[idx]++;
            }
          }
          existing->first_response = resp;
        }
      }
      continue;
    }

    /* New voter */
    VoterInfo *info = g_new0(VoterInfo, 1);
    info->first_response = resp;
    g_hash_table_insert(tally->voter_map,
                        g_strdup(resp->responder_pubkey),
                        info);
    tally->total_voters++;

    /* Count votes */
    for (guint j = 0; j < resp->selected_indices->len; j++) {
      int idx = g_array_index(resp->selected_indices, int, j);
      if (idx >= 0 && (gsize)idx < num_options) {
        tally->vote_counts[idx]++;
      }
    }
  }

  return tally;
}

void gnostr_nip88_vote_tally_free(GnostrNip88VoteTally *tally) {
  if (!tally) return;
  g_free(tally->vote_counts);
  if (tally->voter_map) {
    g_hash_table_destroy(tally->voter_map);
  }
  g_free(tally);
}

gboolean gnostr_nip88_has_voted(GnostrNip88VoteTally *tally,
                                 const char *pubkey_hex) {
  if (!tally || !tally->voter_map || !pubkey_hex) return FALSE;
  return g_hash_table_contains(tally->voter_map, pubkey_hex);
}

GArray *gnostr_nip88_get_voter_choices(GnostrNip88VoteTally *tally,
                                        const char *pubkey_hex) {
  if (!tally || !tally->voter_map || !pubkey_hex) return NULL;

  VoterInfo *info = g_hash_table_lookup(tally->voter_map, pubkey_hex);
  if (!info || !info->first_response || !info->first_response->selected_indices) {
    return NULL;
  }

  /* Copy the indices to a new array */
  GArray *result = g_array_sized_new(FALSE, FALSE, sizeof(int),
                                      info->first_response->selected_indices->len);
  for (guint i = 0; i < info->first_response->selected_indices->len; i++) {
    int idx = g_array_index(info->first_response->selected_indices, int, i);
    g_array_append_val(result, idx);
  }

  return result;
}
