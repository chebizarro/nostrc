/**
 * NIP-90: Data Vending Machines (DVM) Implementation
 *
 * Parsing, building, and management of DVM job requests, results, and feedback.
 */

#include "nip90_dvm.h"
#include <json-glib/json-glib.h>
#include <string.h>
#include <time.h>

/* ============== Memory Management ============== */

void gnostr_dvm_input_free(GnostrDvmInput *input) {
  if (!input) return;
  g_free(input->data);
  g_free(input->relay);
  g_free(input->marker);
}

void gnostr_dvm_param_free(GnostrDvmParam *param) {
  if (!param) return;
  g_free(param->name);
  g_free(param->value);
}

GnostrDvmJobRequest *gnostr_dvm_job_request_new(gint job_type) {
  GnostrDvmJobRequest *request = g_new0(GnostrDvmJobRequest, 1);
  request->job_type = job_type;
  return request;
}

void gnostr_dvm_job_request_free(GnostrDvmJobRequest *request) {
  if (!request) return;

  g_free(request->event_id);
  g_free(request->pubkey);

  /* Free inputs */
  if (request->inputs) {
    for (gsize i = 0; i < request->n_inputs; i++) {
      gnostr_dvm_input_free(&request->inputs[i]);
    }
    g_free(request->inputs);
  }

  g_free(request->output_mime);
  g_free(request->target_pubkey);

  /* Free relays */
  if (request->relays) {
    for (gsize i = 0; i < request->n_relays; i++) {
      g_free(request->relays[i]);
    }
    g_free(request->relays);
  }

  /* Free params */
  if (request->params) {
    for (gsize i = 0; i < request->n_params; i++) {
      gnostr_dvm_param_free(&request->params[i]);
    }
    g_free(request->params);
  }

  g_free(request);
}

void gnostr_dvm_job_result_free(GnostrDvmJobResult *result) {
  if (!result) return;

  g_free(result->event_id);
  g_free(result->pubkey);
  g_free(result->request_id);
  g_free(result->request_relay);
  g_free(result->requester_pubkey);
  g_free(result->content);
  g_free(result->bolt11);

  g_free(result);
}

void gnostr_dvm_job_feedback_free(GnostrDvmJobFeedback *feedback) {
  if (!feedback) return;

  g_free(feedback->event_id);
  g_free(feedback->pubkey);
  g_free(feedback->request_id);
  g_free(feedback->requester_pubkey);
  g_free(feedback->status_extra);
  g_free(feedback->bolt11);
  g_free(feedback->content);

  g_free(feedback);
}

/* ============== Request Building ============== */

void gnostr_dvm_job_request_add_input(GnostrDvmJobRequest *request,
                                       const gchar *data,
                                       GnostrDvmInputType type,
                                       const gchar *relay,
                                       const gchar *marker) {
  if (!request || !data) return;

  gsize new_count = request->n_inputs + 1;
  request->inputs = g_realloc(request->inputs, new_count * sizeof(GnostrDvmInput));

  GnostrDvmInput *input = &request->inputs[request->n_inputs];
  memset(input, 0, sizeof(GnostrDvmInput));

  input->data = g_strdup(data);
  input->type = type;
  input->relay = relay ? g_strdup(relay) : NULL;
  input->marker = marker ? g_strdup(marker) : NULL;

  request->n_inputs = new_count;
}

void gnostr_dvm_job_request_add_param(GnostrDvmJobRequest *request,
                                       const gchar *name,
                                       const gchar *value) {
  if (!request || !name || !value) return;

  gsize new_count = request->n_params + 1;
  request->params = g_realloc(request->params, new_count * sizeof(GnostrDvmParam));

  GnostrDvmParam *param = &request->params[request->n_params];
  memset(param, 0, sizeof(GnostrDvmParam));

  param->name = g_strdup(name);
  param->value = g_strdup(value);

  request->n_params = new_count;
}

void gnostr_dvm_job_request_add_relay(GnostrDvmJobRequest *request,
                                       const gchar *relay_url) {
  if (!request || !relay_url) return;

  gsize new_count = request->n_relays + 1;
  request->relays = g_realloc(request->relays, new_count * sizeof(gchar *));
  request->relays[request->n_relays] = g_strdup(relay_url);
  request->n_relays = new_count;
}

gchar *gnostr_dvm_build_request_tags(const GnostrDvmJobRequest *request) {
  if (!request) return NULL;

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_array(builder);

  /* Input tags: ["i", "<data>", "<type>", "<relay>", "<marker>"] */
  for (gsize i = 0; i < request->n_inputs; i++) {
    const GnostrDvmInput *input = &request->inputs[i];
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "i");
    json_builder_add_string_value(builder, input->data ? input->data : "");
    json_builder_add_string_value(builder, gnostr_dvm_input_type_to_string(input->type));
    if (input->relay || input->marker) {
      json_builder_add_string_value(builder, input->relay ? input->relay : "");
      if (input->marker) {
        json_builder_add_string_value(builder, input->marker);
      }
    }
    json_builder_end_array(builder);
  }

  /* Output tag: ["output", "<mime-type>"] */
  if (request->output_mime && *request->output_mime) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "output");
    json_builder_add_string_value(builder, request->output_mime);
    json_builder_end_array(builder);
  }

  /* Bid tag: ["bid", "<msats>", "<max-msats>"] */
  if (request->bid_msats > 0) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "bid");
    gchar *bid_str = g_strdup_printf("%" G_GINT64_FORMAT, request->bid_msats);
    json_builder_add_string_value(builder, bid_str);
    g_free(bid_str);
    if (request->max_msats > 0) {
      gchar *max_str = g_strdup_printf("%" G_GINT64_FORMAT, request->max_msats);
      json_builder_add_string_value(builder, max_str);
      g_free(max_str);
    }
    json_builder_end_array(builder);
  }

  /* Relays tag: ["relays", "relay1", "relay2", ...] */
  if (request->n_relays > 0) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "relays");
    for (gsize i = 0; i < request->n_relays; i++) {
      json_builder_add_string_value(builder, request->relays[i]);
    }
    json_builder_end_array(builder);
  }

  /* Target provider: ["p", "<pubkey>"] */
  if (request->target_pubkey && *request->target_pubkey) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "p");
    json_builder_add_string_value(builder, request->target_pubkey);
    json_builder_end_array(builder);
  }

  /* Parameter tags: ["param", "<name>", "<value>"] */
  for (gsize i = 0; i < request->n_params; i++) {
    const GnostrDvmParam *param = &request->params[i];
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "param");
    json_builder_add_string_value(builder, param->name ? param->name : "");
    json_builder_add_string_value(builder, param->value ? param->value : "");
    json_builder_end_array(builder);
  }

  json_builder_end_array(builder);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(gen, root);
  json_generator_set_pretty(gen, FALSE);
  gchar *result = json_generator_to_data(gen, NULL);


  return result;
}

gchar *gnostr_dvm_build_request_event(const GnostrDvmJobRequest *request) {
  if (!request) return NULL;

  if (request->n_inputs == 0) {
    g_warning("NIP-90: Cannot create request without inputs");
    return NULL;
  }

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);

  /* Kind: 5000 + job_type */
  gint kind = gnostr_dvm_request_kind_for_job(request->job_type);
  json_builder_set_member_name(builder, "kind");
  json_builder_add_int_value(builder, kind);

  /* Content - typically empty for requests, but could contain encrypted data */
  json_builder_set_member_name(builder, "content");
  json_builder_add_string_value(builder, "");

  /* Created at */
  json_builder_set_member_name(builder, "created_at");
  json_builder_add_int_value(builder, (gint64)time(NULL));

  /* Tags */
  json_builder_set_member_name(builder, "tags");
  json_builder_begin_array(builder);

  /* Input tags */
  for (gsize i = 0; i < request->n_inputs; i++) {
    const GnostrDvmInput *input = &request->inputs[i];
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "i");
    json_builder_add_string_value(builder, input->data ? input->data : "");
    json_builder_add_string_value(builder, gnostr_dvm_input_type_to_string(input->type));
    if (input->relay || input->marker) {
      json_builder_add_string_value(builder, input->relay ? input->relay : "");
      if (input->marker) {
        json_builder_add_string_value(builder, input->marker);
      }
    }
    json_builder_end_array(builder);
  }

  /* Output tag */
  if (request->output_mime && *request->output_mime) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "output");
    json_builder_add_string_value(builder, request->output_mime);
    json_builder_end_array(builder);
  }

  /* Bid tag */
  if (request->bid_msats > 0) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "bid");
    gchar *bid_str = g_strdup_printf("%" G_GINT64_FORMAT, request->bid_msats);
    json_builder_add_string_value(builder, bid_str);
    g_free(bid_str);
    if (request->max_msats > 0) {
      gchar *max_str = g_strdup_printf("%" G_GINT64_FORMAT, request->max_msats);
      json_builder_add_string_value(builder, max_str);
      g_free(max_str);
    }
    json_builder_end_array(builder);
  }

  /* Relays tag */
  if (request->n_relays > 0) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "relays");
    for (gsize i = 0; i < request->n_relays; i++) {
      json_builder_add_string_value(builder, request->relays[i]);
    }
    json_builder_end_array(builder);
  }

  /* Target provider */
  if (request->target_pubkey && *request->target_pubkey) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "p");
    json_builder_add_string_value(builder, request->target_pubkey);
    json_builder_end_array(builder);
  }

  /* Parameters */
  for (gsize i = 0; i < request->n_params; i++) {
    const GnostrDvmParam *param = &request->params[i];
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "param");
    json_builder_add_string_value(builder, param->name ? param->name : "");
    json_builder_add_string_value(builder, param->value ? param->value : "");
    json_builder_end_array(builder);
  }

  json_builder_end_array(builder);  /* tags */
  json_builder_end_object(builder);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(gen, root);
  json_generator_set_pretty(gen, FALSE);
  gchar *result = json_generator_to_data(gen, NULL);


  return result;
}

/* ============== Parsing ============== */

GnostrDvmInputType gnostr_dvm_parse_input_type(const gchar *type_str) {
  if (!type_str) return GNOSTR_DVM_INPUT_UNKNOWN;

  if (g_strcmp0(type_str, "text") == 0) {
    return GNOSTR_DVM_INPUT_TEXT;
  } else if (g_strcmp0(type_str, "url") == 0) {
    return GNOSTR_DVM_INPUT_URL;
  } else if (g_strcmp0(type_str, "event") == 0) {
    return GNOSTR_DVM_INPUT_EVENT;
  } else if (g_strcmp0(type_str, "job") == 0) {
    return GNOSTR_DVM_INPUT_JOB;
  }

  return GNOSTR_DVM_INPUT_UNKNOWN;
}

const gchar *gnostr_dvm_input_type_to_string(GnostrDvmInputType type) {
  switch (type) {
    case GNOSTR_DVM_INPUT_TEXT:
      return "text";
    case GNOSTR_DVM_INPUT_URL:
      return "url";
    case GNOSTR_DVM_INPUT_EVENT:
      return "event";
    case GNOSTR_DVM_INPUT_JOB:
      return "job";
    default:
      return "text";  /* Default to text */
  }
}

GnostrDvmJobStatus gnostr_dvm_parse_status(const gchar *status_str) {
  if (!status_str) return GNOSTR_DVM_STATUS_UNKNOWN;

  if (g_strcmp0(status_str, "processing") == 0) {
    return GNOSTR_DVM_STATUS_PROCESSING;
  } else if (g_strcmp0(status_str, "error") == 0) {
    return GNOSTR_DVM_STATUS_ERROR;
  } else if (g_strcmp0(status_str, "success") == 0) {
    return GNOSTR_DVM_STATUS_SUCCESS;
  } else if (g_strcmp0(status_str, "partial") == 0) {
    return GNOSTR_DVM_STATUS_PARTIAL;
  } else if (g_strcmp0(status_str, "payment-required") == 0) {
    return GNOSTR_DVM_STATUS_PAYMENT_REQUIRED;
  }

  return GNOSTR_DVM_STATUS_UNKNOWN;
}

const gchar *gnostr_dvm_status_to_string(GnostrDvmJobStatus status) {
  switch (status) {
    case GNOSTR_DVM_STATUS_PROCESSING:
      return "processing";
    case GNOSTR_DVM_STATUS_ERROR:
      return "error";
    case GNOSTR_DVM_STATUS_SUCCESS:
      return "success";
    case GNOSTR_DVM_STATUS_PARTIAL:
      return "partial";
    case GNOSTR_DVM_STATUS_PAYMENT_REQUIRED:
      return "payment-required";
    default:
      return "unknown";
  }
}

GnostrDvmJobRequest *gnostr_dvm_job_request_parse(const gchar *json_str) {
  if (!json_str || !*json_str) return NULL;

  g_autoptr(JsonParser) parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, json_str, -1, &error)) {
    g_debug("NIP-90: Failed to parse request JSON: %s", error ? error->message : "unknown");
    g_clear_error(&error);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    return NULL;
  }

  JsonObject *obj = json_node_get_object(root);

  /* Check kind is in request range */
  if (!json_object_has_member(obj, "kind")) {
    return NULL;
  }

  gint64 kind = json_object_get_int_member(obj, "kind");
  if (!gnostr_dvm_is_request_kind((gint)kind)) {
    return NULL;
  }

  gint job_type = gnostr_dvm_job_type_from_kind((gint)kind);
  GnostrDvmJobRequest *request = gnostr_dvm_job_request_new(job_type);

  /* Get event ID */
  if (json_object_has_member(obj, "id")) {
    request->event_id = g_strdup(json_object_get_string_member(obj, "id"));
  }

  /* Get pubkey */
  if (json_object_has_member(obj, "pubkey")) {
    request->pubkey = g_strdup(json_object_get_string_member(obj, "pubkey"));
  }

  /* Get created_at */
  if (json_object_has_member(obj, "created_at")) {
    request->created_at = json_object_get_int_member(obj, "created_at");
  }

  /* Temporary arrays for parsing */
  GPtrArray *inputs_arr = g_ptr_array_new();
  GPtrArray *params_arr = g_ptr_array_new();
  GPtrArray *relays_arr = g_ptr_array_new();

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

      const gchar *tag_name = json_array_get_string_element(tag, 0);
      if (!tag_name) continue;

      if (g_strcmp0(tag_name, "i") == 0 && tag_len >= 3) {
        /* Input: ["i", "<data>", "<type>", "<relay>", "<marker>"] */
        GnostrDvmInput *input = g_new0(GnostrDvmInput, 1);
        input->data = g_strdup(json_array_get_string_element(tag, 1));
        input->type = gnostr_dvm_parse_input_type(json_array_get_string_element(tag, 2));
        if (tag_len >= 4) {
          const gchar *relay = json_array_get_string_element(tag, 3);
          if (relay && *relay) {
            input->relay = g_strdup(relay);
          }
        }
        if (tag_len >= 5) {
          const gchar *marker = json_array_get_string_element(tag, 4);
          if (marker && *marker) {
            input->marker = g_strdup(marker);
          }
        }
        g_ptr_array_add(inputs_arr, input);

      } else if (g_strcmp0(tag_name, "output") == 0) {
        /* Output: ["output", "<mime-type>"] */
        g_free(request->output_mime);
        request->output_mime = g_strdup(json_array_get_string_element(tag, 1));

      } else if (g_strcmp0(tag_name, "bid") == 0) {
        /* Bid: ["bid", "<msats>", "<max-msats>"] */
        const gchar *bid_str = json_array_get_string_element(tag, 1);
        if (bid_str) {
          request->bid_msats = g_ascii_strtoll(bid_str, NULL, 10);
        }
        if (tag_len >= 3) {
          const gchar *max_str = json_array_get_string_element(tag, 2);
          if (max_str) {
            request->max_msats = g_ascii_strtoll(max_str, NULL, 10);
          }
        }

      } else if (g_strcmp0(tag_name, "relays") == 0) {
        /* Relays: ["relays", "wss://...", "wss://..."] */
        for (guint j = 1; j < tag_len; j++) {
          const gchar *relay = json_array_get_string_element(tag, j);
          if (relay && *relay) {
            g_ptr_array_add(relays_arr, g_strdup(relay));
          }
        }

      } else if (g_strcmp0(tag_name, "p") == 0) {
        /* Target provider: ["p", "<pubkey>"] */
        g_free(request->target_pubkey);
        request->target_pubkey = g_strdup(json_array_get_string_element(tag, 1));

      } else if (g_strcmp0(tag_name, "param") == 0 && tag_len >= 3) {
        /* Parameter: ["param", "<name>", "<value>"] */
        GnostrDvmParam *param = g_new0(GnostrDvmParam, 1);
        param->name = g_strdup(json_array_get_string_element(tag, 1));
        param->value = g_strdup(json_array_get_string_element(tag, 2));
        g_ptr_array_add(params_arr, param);
      }
    }
  }

  /* Convert inputs array */
  if (inputs_arr->len > 0) {
    request->n_inputs = inputs_arr->len;
    request->inputs = g_new0(GnostrDvmInput, request->n_inputs);
    for (gsize i = 0; i < inputs_arr->len; i++) {
      GnostrDvmInput *src = g_ptr_array_index(inputs_arr, i);
      request->inputs[i] = *src;
      g_free(src);  /* Free the container, not the contents */
    }
  }
  g_ptr_array_free(inputs_arr, TRUE);

  /* Convert params array */
  if (params_arr->len > 0) {
    request->n_params = params_arr->len;
    request->params = g_new0(GnostrDvmParam, request->n_params);
    for (gsize i = 0; i < params_arr->len; i++) {
      GnostrDvmParam *src = g_ptr_array_index(params_arr, i);
      request->params[i] = *src;
      g_free(src);
    }
  }
  g_ptr_array_free(params_arr, TRUE);

  /* Convert relays array */
  if (relays_arr->len > 0) {
    request->n_relays = relays_arr->len;
    request->relays = g_new0(gchar *, request->n_relays);
    for (gsize i = 0; i < relays_arr->len; i++) {
      request->relays[i] = g_ptr_array_index(relays_arr, i);
    }
  }
  g_ptr_array_free(relays_arr, TRUE);


  return request;
}

GnostrDvmJobResult *gnostr_dvm_job_result_parse(const gchar *json_str) {
  if (!json_str || !*json_str) return NULL;

  g_autoptr(JsonParser) parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, json_str, -1, &error)) {
    g_debug("NIP-90: Failed to parse result JSON: %s", error ? error->message : "unknown");
    g_clear_error(&error);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    return NULL;
  }

  JsonObject *obj = json_node_get_object(root);

  /* Check kind is in result range */
  if (!json_object_has_member(obj, "kind")) {
    return NULL;
  }

  gint64 kind = json_object_get_int_member(obj, "kind");
  if (!gnostr_dvm_is_result_kind((gint)kind)) {
    return NULL;
  }

  GnostrDvmJobResult *result = g_new0(GnostrDvmJobResult, 1);
  result->job_type = gnostr_dvm_job_type_from_kind((gint)kind);
  result->status = GNOSTR_DVM_STATUS_SUCCESS;  /* Default for result events */

  /* Get event ID */
  if (json_object_has_member(obj, "id")) {
    result->event_id = g_strdup(json_object_get_string_member(obj, "id"));
  }

  /* Get pubkey (DVM service provider) */
  if (json_object_has_member(obj, "pubkey")) {
    result->pubkey = g_strdup(json_object_get_string_member(obj, "pubkey"));
  }

  /* Get created_at */
  if (json_object_has_member(obj, "created_at")) {
    result->created_at = json_object_get_int_member(obj, "created_at");
  }

  /* Get content (result data) */
  if (json_object_has_member(obj, "content")) {
    result->content = g_strdup(json_object_get_string_member(obj, "content"));
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

      const gchar *tag_name = json_array_get_string_element(tag, 0);
      if (!tag_name) continue;

      if (g_strcmp0(tag_name, "e") == 0) {
        /* Request reference: ["e", "<request-id>", "<relay>"] */
        g_free(result->request_id);
        result->request_id = g_strdup(json_array_get_string_element(tag, 1));
        if (tag_len >= 3) {
          g_free(result->request_relay);
          const gchar *relay = json_array_get_string_element(tag, 2);
          if (relay && *relay) {
            result->request_relay = g_strdup(relay);
          }
        }

      } else if (g_strcmp0(tag_name, "p") == 0) {
        /* Requester pubkey: ["p", "<pubkey>"] */
        g_free(result->requester_pubkey);
        result->requester_pubkey = g_strdup(json_array_get_string_element(tag, 1));

      } else if (g_strcmp0(tag_name, "amount") == 0) {
        /* Payment info: ["amount", "<msats>", "<bolt11>"] */
        const gchar *amount_str = json_array_get_string_element(tag, 1);
        if (amount_str) {
          result->amount_msats = g_ascii_strtoll(amount_str, NULL, 10);
        }
        if (tag_len >= 3) {
          g_free(result->bolt11);
          result->bolt11 = g_strdup(json_array_get_string_element(tag, 2));
        }

      } else if (g_strcmp0(tag_name, "status") == 0) {
        /* Status: ["status", "<status>", "<extra>"] */
        result->status = gnostr_dvm_parse_status(json_array_get_string_element(tag, 1));

      } else if (g_strcmp0(tag_name, "encrypted") == 0) {
        /* Encrypted flag */
        result->encrypted = TRUE;
      }
    }
  }


  return result;
}

GnostrDvmJobFeedback *gnostr_dvm_job_feedback_parse(const gchar *json_str) {
  if (!json_str || !*json_str) return NULL;

  g_autoptr(JsonParser) parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, json_str, -1, &error)) {
    g_debug("NIP-90: Failed to parse feedback JSON: %s", error ? error->message : "unknown");
    g_clear_error(&error);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    return NULL;
  }

  JsonObject *obj = json_node_get_object(root);

  /* Check kind is feedback (7000) */
  if (!json_object_has_member(obj, "kind")) {
    return NULL;
  }

  gint64 kind = json_object_get_int_member(obj, "kind");
  if (!gnostr_dvm_is_feedback_kind((gint)kind)) {
    return NULL;
  }

  GnostrDvmJobFeedback *feedback = g_new0(GnostrDvmJobFeedback, 1);
  feedback->progress_percent = -1;  /* -1 = not available */

  /* Get event ID */
  if (json_object_has_member(obj, "id")) {
    feedback->event_id = g_strdup(json_object_get_string_member(obj, "id"));
  }

  /* Get pubkey (DVM service provider) */
  if (json_object_has_member(obj, "pubkey")) {
    feedback->pubkey = g_strdup(json_object_get_string_member(obj, "pubkey"));
  }

  /* Get created_at */
  if (json_object_has_member(obj, "created_at")) {
    feedback->created_at = json_object_get_int_member(obj, "created_at");
  }

  /* Get content (optional status message or partial results) */
  if (json_object_has_member(obj, "content")) {
    feedback->content = g_strdup(json_object_get_string_member(obj, "content"));
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

      const gchar *tag_name = json_array_get_string_element(tag, 0);
      if (!tag_name) continue;

      if (g_strcmp0(tag_name, "status") == 0) {
        /* Status: ["status", "<status>", "<extra-info>"] */
        feedback->status = gnostr_dvm_parse_status(json_array_get_string_element(tag, 1));
        if (tag_len >= 3) {
          g_free(feedback->status_extra);
          feedback->status_extra = g_strdup(json_array_get_string_element(tag, 2));
        }

      } else if (g_strcmp0(tag_name, "e") == 0) {
        /* Request reference: ["e", "<request-id>"] */
        g_free(feedback->request_id);
        feedback->request_id = g_strdup(json_array_get_string_element(tag, 1));

      } else if (g_strcmp0(tag_name, "p") == 0) {
        /* Requester pubkey: ["p", "<pubkey>"] */
        g_free(feedback->requester_pubkey);
        feedback->requester_pubkey = g_strdup(json_array_get_string_element(tag, 1));

      } else if (g_strcmp0(tag_name, "amount") == 0) {
        /* Payment info: ["amount", "<msats>", "<bolt11>"] */
        const gchar *amount_str = json_array_get_string_element(tag, 1);
        if (amount_str) {
          feedback->amount_msats = g_ascii_strtoll(amount_str, NULL, 10);
        }
        if (tag_len >= 3) {
          g_free(feedback->bolt11);
          feedback->bolt11 = g_strdup(json_array_get_string_element(tag, 2));
        }

      } else if (g_strcmp0(tag_name, "progress") == 0) {
        /* Progress: ["progress", "<percent>"] */
        const gchar *progress_str = json_array_get_string_element(tag, 1);
        if (progress_str) {
          feedback->progress_percent = (gint)g_ascii_strtoll(progress_str, NULL, 10);
          if (feedback->progress_percent < 0) feedback->progress_percent = 0;
          if (feedback->progress_percent > 100) feedback->progress_percent = 100;
        }
      }
    }
  }


  return feedback;
}

/* ============== Kind Helpers ============== */

gboolean gnostr_dvm_is_request_kind(gint kind) {
  return kind >= GNOSTR_DVM_KIND_REQUEST_MIN && kind <= GNOSTR_DVM_KIND_REQUEST_MAX;
}

gboolean gnostr_dvm_is_result_kind(gint kind) {
  return kind >= GNOSTR_DVM_KIND_RESULT_MIN && kind <= GNOSTR_DVM_KIND_RESULT_MAX;
}

gboolean gnostr_dvm_is_feedback_kind(gint kind) {
  return kind == GNOSTR_DVM_KIND_FEEDBACK;
}

gint gnostr_dvm_request_kind_for_job(gint job_type) {
  return GNOSTR_DVM_KIND_REQUEST_MIN + job_type;
}

gint gnostr_dvm_result_kind_for_job(gint job_type) {
  return GNOSTR_DVM_KIND_RESULT_MIN + job_type;
}

gint gnostr_dvm_job_type_from_kind(gint kind) {
  if (gnostr_dvm_is_request_kind(kind)) {
    return kind - GNOSTR_DVM_KIND_REQUEST_MIN;
  } else if (gnostr_dvm_is_result_kind(kind)) {
    return kind - GNOSTR_DVM_KIND_RESULT_MIN;
  }
  return -1;
}

const gchar *gnostr_dvm_get_job_type_name(gint job_type) {
  switch (job_type) {
    case GNOSTR_DVM_JOB_TEXT_TRANSLATION:
      return "Text Translation";
    case GNOSTR_DVM_JOB_TEXT_SUMMARIZATION:
      return "Text Summarization";
    case GNOSTR_DVM_JOB_IMAGE_GENERATION:
      return "Image Generation";
    case GNOSTR_DVM_JOB_TEXT_TO_SPEECH:
      return "Text-to-Speech";
    case GNOSTR_DVM_JOB_SPEECH_TO_TEXT:
      return "Speech-to-Text";
    case GNOSTR_DVM_JOB_CONTENT_DISCOVERY:
      return "Content Discovery";
    default:
      return "Unknown Job Type";
  }
}

/* ============== Filter Building ============== */

gchar *gnostr_dvm_build_request_filter(gint job_type, gint64 since, gint limit) {
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);

  /* Kinds */
  json_builder_set_member_name(builder, "kinds");
  json_builder_begin_array(builder);
  if (job_type >= 0) {
    /* Specific job type */
    json_builder_add_int_value(builder, gnostr_dvm_request_kind_for_job(job_type));
  } else {
    /* All request types - add common ones */
    for (gint i = 0; i <= 5; i++) {
      json_builder_add_int_value(builder, GNOSTR_DVM_KIND_REQUEST_MIN + i);
    }
  }
  json_builder_end_array(builder);

  /* Since */
  if (since > 0) {
    json_builder_set_member_name(builder, "since");
    json_builder_add_int_value(builder, since);
  }

  /* Limit */
  if (limit > 0) {
    json_builder_set_member_name(builder, "limit");
    json_builder_add_int_value(builder, limit);
  }

  json_builder_end_object(builder);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(gen, root);
  json_generator_set_pretty(gen, FALSE);
  gchar *result = json_generator_to_data(gen, NULL);


  return result;
}

gchar *gnostr_dvm_build_result_filter(const gchar *request_id,
                                       gint job_type,
                                       gint64 since,
                                       gint limit) {
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);

  /* Kinds */
  json_builder_set_member_name(builder, "kinds");
  json_builder_begin_array(builder);
  if (job_type >= 0) {
    json_builder_add_int_value(builder, gnostr_dvm_result_kind_for_job(job_type));
  } else {
    /* All result types - add common ones */
    for (gint i = 0; i <= 5; i++) {
      json_builder_add_int_value(builder, GNOSTR_DVM_KIND_RESULT_MIN + i);
    }
  }
  json_builder_end_array(builder);

  /* Filter by request ID using #e tag */
  if (request_id && *request_id) {
    json_builder_set_member_name(builder, "#e");
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, request_id);
    json_builder_end_array(builder);
  }

  /* Since */
  if (since > 0) {
    json_builder_set_member_name(builder, "since");
    json_builder_add_int_value(builder, since);
  }

  /* Limit */
  if (limit > 0) {
    json_builder_set_member_name(builder, "limit");
    json_builder_add_int_value(builder, limit);
  }

  json_builder_end_object(builder);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(gen, root);
  json_generator_set_pretty(gen, FALSE);
  gchar *result = json_generator_to_data(gen, NULL);


  return result;
}

gchar *gnostr_dvm_build_feedback_filter(const gchar *request_id,
                                         gint64 since,
                                         gint limit) {
  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);

  /* Kind 7000 */
  json_builder_set_member_name(builder, "kinds");
  json_builder_begin_array(builder);
  json_builder_add_int_value(builder, GNOSTR_DVM_KIND_FEEDBACK);
  json_builder_end_array(builder);

  /* Filter by request ID using #e tag */
  if (request_id && *request_id) {
    json_builder_set_member_name(builder, "#e");
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, request_id);
    json_builder_end_array(builder);
  }

  /* Since */
  if (since > 0) {
    json_builder_set_member_name(builder, "since");
    json_builder_add_int_value(builder, since);
  }

  /* Limit */
  if (limit > 0) {
    json_builder_set_member_name(builder, "limit");
    json_builder_add_int_value(builder, limit);
  }

  json_builder_end_object(builder);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(gen, root);
  json_generator_set_pretty(gen, FALSE);
  gchar *result = json_generator_to_data(gen, NULL);


  return result;
}
