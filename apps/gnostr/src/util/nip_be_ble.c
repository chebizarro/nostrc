/**
 * @file nip_be_ble.c
 * @brief NIP-BE (190) BLE Communications Utilities Implementation
 */

#include "nip_be_ble.h"
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

GnostrBleMessage *gnostr_ble_message_new(void) {
  GnostrBleMessage *msg = g_new0(GnostrBleMessage, 1);
  msg->mtu = 0;
  msg->created_at = 0;
  return msg;
}

void gnostr_ble_message_free(GnostrBleMessage *msg) {
  if (!msg) return;

  g_free(msg->device_uuid);
  g_free(msg->service_uuid);
  g_free(msg->char_uuid);
  g_free(msg->content);
  g_free(msg->recipient);
  g_free(msg->related_event);

  g_free(msg);
}

GnostrBleMessage *gnostr_ble_message_copy(const GnostrBleMessage *msg) {
  if (!msg) return NULL;

  GnostrBleMessage *copy = gnostr_ble_message_new();
  copy->device_uuid = g_strdup(msg->device_uuid);
  copy->service_uuid = g_strdup(msg->service_uuid);
  copy->char_uuid = g_strdup(msg->char_uuid);
  copy->mtu = msg->mtu;
  copy->content = g_strdup(msg->content);
  copy->recipient = g_strdup(msg->recipient);
  copy->related_event = g_strdup(msg->related_event);
  copy->created_at = msg->created_at;

  return copy;
}

GnostrBleMessage *gnostr_ble_message_parse(const char *tags_json,
                                            const char *content) {
  if (!tags_json || !*tags_json) return NULL;

  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, tags_json, -1, &error)) {
    g_warning("NIP-BE: Failed to parse tags JSON: %s", error->message);
    g_error_free(error);
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_ARRAY(root)) {
    g_warning("NIP-BE: Tags is not an array");
    g_object_unref(parser);
    return NULL;
  }

  JsonArray *tags = json_node_get_array(root);
  guint n_tags = json_array_get_length(tags);

  GnostrBleMessage *msg = gnostr_ble_message_new();

  /* Set content from event content if provided */
  if (content && *content) {
    msg->content = g_strdup(content);
  }

  for (guint i = 0; i < n_tags; i++) {
    JsonNode *tag_node = json_array_get_element(tags, i);
    if (!JSON_NODE_HOLDS_ARRAY(tag_node)) continue;

    JsonArray *tag = json_node_get_array(tag_node);
    guint tag_len = json_array_get_length(tag);
    if (tag_len < 2) continue;

    const char *tag_name = json_array_get_string_element(tag, 0);
    const char *tag_value = json_array_get_string_element(tag, 1);

    if (!tag_name || !tag_value) continue;

    if (strcmp(tag_name, "ble-id") == 0) {
      /* BLE device identifier (required) */
      g_free(msg->device_uuid);
      msg->device_uuid = gnostr_ble_normalize_uuid(tag_value);

    } else if (strcmp(tag_name, "service") == 0) {
      /* BLE service UUID */
      g_free(msg->service_uuid);
      msg->service_uuid = gnostr_ble_normalize_uuid(tag_value);

    } else if (strcmp(tag_name, "characteristic") == 0) {
      /* BLE characteristic UUID */
      g_free(msg->char_uuid);
      msg->char_uuid = gnostr_ble_normalize_uuid(tag_value);

    } else if (strcmp(tag_name, "mtu") == 0) {
      /* Negotiated MTU size */
      char *endptr;
      gint mtu = (gint)g_ascii_strtoll(tag_value, &endptr, 10);
      if (endptr != tag_value && gnostr_ble_validate_mtu(mtu)) {
        msg->mtu = mtu;
      }

    } else if (strcmp(tag_name, "p") == 0) {
      /* Target recipient pubkey - only set if not already set */
      if (!msg->recipient && strlen(tag_value) == 64) {
        msg->recipient = g_strdup(tag_value);
      }

    } else if (strcmp(tag_name, "e") == 0) {
      /* Related event ID - only set if not already set */
      if (!msg->related_event && strlen(tag_value) == 64) {
        msg->related_event = g_strdup(tag_value);
      }
    }
  }

  g_object_unref(parser);

  /* Validate required fields - device UUID is required */
  if (!msg->device_uuid || !*msg->device_uuid) {
    g_warning("NIP-BE: BLE message missing required 'ble-id' tag");
    gnostr_ble_message_free(msg);
    return NULL;
  }

  /* Validate device UUID format */
  if (!gnostr_ble_validate_uuid(msg->device_uuid)) {
    g_warning("NIP-BE: BLE message has invalid device UUID: %s", msg->device_uuid);
    gnostr_ble_message_free(msg);
    return NULL;
  }

  return msg;
}

char *gnostr_ble_message_build_tags(const GnostrBleMessage *msg) {
  if (!msg || !msg->device_uuid) return NULL;

  JsonBuilder *builder = json_builder_new();
  json_builder_begin_array(builder);

  /* ble-id tag (required) */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "ble-id");
  json_builder_add_string_value(builder, msg->device_uuid);
  json_builder_end_array(builder);

  /* Service UUID tag */
  if (msg->service_uuid && *msg->service_uuid) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "service");
    json_builder_add_string_value(builder, msg->service_uuid);
    json_builder_end_array(builder);
  }

  /* Characteristic UUID tag */
  if (msg->char_uuid && *msg->char_uuid) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "characteristic");
    json_builder_add_string_value(builder, msg->char_uuid);
    json_builder_end_array(builder);
  }

  /* MTU tag */
  if (msg->mtu > 0) {
    char mtu_str[16];
    g_snprintf(mtu_str, sizeof(mtu_str), "%d", msg->mtu);
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "mtu");
    json_builder_add_string_value(builder, mtu_str);
    json_builder_end_array(builder);
  }

  /* Recipient pubkey tag */
  if (msg->recipient && *msg->recipient) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "p");
    json_builder_add_string_value(builder, msg->recipient);
    json_builder_end_array(builder);
  }

  /* Related event tag */
  if (msg->related_event && *msg->related_event) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "e");
    json_builder_add_string_value(builder, msg->related_event);
    json_builder_end_array(builder);
  }

  json_builder_end_array(builder);

  JsonGenerator *generator = json_generator_new();
  JsonNode *root_node = json_builder_get_root(builder);
  json_generator_set_root(generator, root_node);

  char *tags_json = json_generator_to_data(generator, NULL);

  json_node_free(root_node);
  g_object_unref(generator);
  g_object_unref(builder);

  return tags_json;
}

gboolean gnostr_ble_is_ble(int kind) {
  return kind == NIPBE_KIND_BLE;
}

gboolean gnostr_ble_validate_uuid(const char *uuid) {
  if (!uuid || !*uuid) return FALSE;

  size_t len = strlen(uuid);

  /* Check for standard 8-4-4-4-12 format (36 chars with hyphens) */
  if (len == 36) {
    /* Validate format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */
    for (size_t i = 0; i < len; i++) {
      if (i == 8 || i == 13 || i == 18 || i == 23) {
        if (uuid[i] != '-') return FALSE;
      } else {
        if (!isxdigit((unsigned char)uuid[i])) return FALSE;
      }
    }
    return TRUE;
  }

  /* Check for 32-char hex format (no hyphens) */
  if (len == 32) {
    for (size_t i = 0; i < len; i++) {
      if (!isxdigit((unsigned char)uuid[i])) return FALSE;
    }
    return TRUE;
  }

  return FALSE;
}

gchar *gnostr_ble_normalize_uuid(const char *uuid) {
  if (!uuid || !*uuid) return NULL;

  size_t len = strlen(uuid);
  gchar *result = NULL;

  if (len == 36) {
    /* Already in 8-4-4-4-12 format, just lowercase it */
    if (!gnostr_ble_validate_uuid(uuid)) return NULL;
    result = g_ascii_strdown(uuid, -1);

  } else if (len == 32) {
    /* Convert 32-char hex to 8-4-4-4-12 format */
    if (!gnostr_ble_validate_uuid(uuid)) return NULL;

    gchar *lower = g_ascii_strdown(uuid, -1);
    result = g_strdup_printf("%.8s-%.4s-%.4s-%.4s-%.12s",
                             lower,
                             lower + 8,
                             lower + 12,
                             lower + 16,
                             lower + 20);
    g_free(lower);

  } else {
    return NULL;
  }

  return result;
}

gboolean gnostr_ble_validate_mtu(gint mtu) {
  /* BLE 4.0 minimum ATT MTU is 23 bytes */
  /* BLE 4.2+ maximum MTU is 512 bytes */
  return mtu >= GNOSTR_BLE_MTU_DEFAULT && mtu <= GNOSTR_BLE_MTU_MAX;
}

gint gnostr_ble_get_max_payload(gint mtu) {
  if (mtu < GNOSTR_BLE_MTU_DEFAULT) {
    mtu = GNOSTR_BLE_MTU_DEFAULT;
  }
  /* ATT protocol overhead is 3 bytes (opcode + handle) */
  return mtu - 3;
}

int gnostr_ble_get_kind(void) {
  return NIPBE_KIND_BLE;
}

const char *gnostr_ble_get_service_uuid(void) {
  return GNOSTR_BLE_SERVICE_UUID;
}

const char *gnostr_ble_get_npub_char_uuid(void) {
  return GNOSTR_BLE_CHAR_NPUB_UUID;
}

const char *gnostr_ble_get_message_char_uuid(void) {
  return GNOSTR_BLE_CHAR_MESSAGE_UUID;
}

const char *gnostr_ble_get_event_char_uuid(void) {
  return GNOSTR_BLE_CHAR_EVENT_UUID;
}
