/* native_messaging.c - NIP-07 browser extension native messaging implementation
 *
 * Implements Chrome/Firefox native messaging protocol for NIP-07.
 *
 * Protocol:
 * - Input: 4-byte LE length prefix + JSON message
 * - Output: 4-byte LE length prefix + JSON response
 *
 * Security considerations:
 * - Message size limited to 1MB (Chrome's native messaging limit)
 * - Uses secure memory for secret key operations
 * - Core dumps disabled at startup
 */
#include "native_messaging.h"

#include <nostr/nip55l/signer_ops.h>
#include <nostr/nip19/nip19.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

struct _NativeMessagingContext {
  gchar *identity;                    /* npub or key_id for signing */
  NativeMessagingAuthorizeCb auth_cb; /* Authorization callback */
  gpointer auth_cb_ud;                /* User data for auth callback */
  GArray *auto_approve_kinds;         /* Event kinds to auto-approve */
};

/* Error messages */
static const gchar *error_messages[] = {
  [NM_OK]                 = "Success",
  [NM_ERR_IO]             = "I/O error",
  [NM_ERR_MSG_TOO_LARGE]  = "Message too large",
  [NM_ERR_INVALID_JSON]   = "Invalid JSON",
  [NM_ERR_INVALID_REQUEST]= "Invalid request",
  [NM_ERR_UNKNOWN_METHOD] = "Unknown method",
  [NM_ERR_USER_DENIED]    = "User denied the request",
  [NM_ERR_NO_KEY]         = "No key available",
  [NM_ERR_SIGN_FAILED]    = "Signing failed",
  [NM_ERR_ENCRYPT_FAILED] = "Encryption failed",
  [NM_ERR_DECRYPT_FAILED] = "Decryption failed",
  [NM_ERR_INTERNAL]       = "Internal error"
};

const gchar *native_messaging_error_message(NativeMessagingError code) {
  if (code >= 0 && (gsize)code < G_N_ELEMENTS(error_messages)) {
    return error_messages[code];
  }
  return "Unknown error";
}

NativeMessagingContext *native_messaging_context_new(const gchar *identity) {
  NativeMessagingContext *ctx = g_new0(NativeMessagingContext, 1);
  ctx->identity = identity ? g_strdup(identity) : NULL;
  ctx->auto_approve_kinds = g_array_new(FALSE, FALSE, sizeof(gint));
  return ctx;
}

void native_messaging_context_free(NativeMessagingContext *ctx) {
  if (!ctx) return;
  g_free(ctx->identity);
  if (ctx->auto_approve_kinds) {
    g_array_free(ctx->auto_approve_kinds, TRUE);
  }
  g_free(ctx);
}

void native_messaging_set_authorize_cb(NativeMessagingContext *ctx,
                                       NativeMessagingAuthorizeCb cb,
                                       gpointer user_data) {
  if (!ctx) return;
  ctx->auth_cb = cb;
  ctx->auth_cb_ud = user_data;
}

void native_messaging_set_auto_approve_kinds(NativeMessagingContext *ctx,
                                             const gint *kinds) {
  if (!ctx || !kinds) return;

  g_array_set_size(ctx->auto_approve_kinds, 0);
  for (const gint *k = kinds; *k != 0; k++) {
    g_array_append_val(ctx->auto_approve_kinds, *k);
  }
}

/* Read exactly n bytes from stdin */
static gboolean read_exact(void *buf, gsize n) {
  guint8 *p = (guint8 *)buf;
  gsize remaining = n;

  while (remaining > 0) {
    gssize rd = read(STDIN_FILENO, p, remaining);
    if (rd < 0) {
      if (errno == EINTR) continue;
      return FALSE;
    }
    if (rd == 0) {
      /* EOF */
      return FALSE;
    }
    p += rd;
    remaining -= (gsize)rd;
  }
  return TRUE;
}

/* Write exactly n bytes to stdout */
static gboolean write_exact(const void *buf, gsize n) {
  const guint8 *p = (const guint8 *)buf;
  gsize remaining = n;

  while (remaining > 0) {
    gssize wr = write(STDOUT_FILENO, p, remaining);
    if (wr < 0) {
      if (errno == EINTR) continue;
      return FALSE;
    }
    p += wr;
    remaining -= (gsize)wr;
  }
  return TRUE;
}

gchar *native_messaging_read_message(NativeMessagingError *out_error) {
#ifdef _WIN32
  /* Set stdin/stdout to binary mode on Windows */
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif

  /* Read 4-byte length prefix (little-endian) */
  guint8 len_buf[4];
  if (!read_exact(len_buf, 4)) {
    if (out_error) *out_error = NM_ERR_IO;
    return NULL;
  }

  guint32 len = ((guint32)len_buf[0]) |
                ((guint32)len_buf[1] << 8) |
                ((guint32)len_buf[2] << 16) |
                ((guint32)len_buf[3] << 24);

  if (len > NATIVE_MESSAGING_MAX_SIZE) {
    if (out_error) *out_error = NM_ERR_MSG_TOO_LARGE;
    return NULL;
  }

  if (len == 0) {
    if (out_error) *out_error = NM_ERR_INVALID_JSON;
    return NULL;
  }

  /* Read message body */
  gchar *msg = g_malloc(len + 1);
  if (!read_exact(msg, len)) {
    g_free(msg);
    if (out_error) *out_error = NM_ERR_IO;
    return NULL;
  }
  msg[len] = '\0';

  if (out_error) *out_error = NM_OK;
  return msg;
}

NativeMessagingError native_messaging_write_json(const gchar *json) {
  if (!json) return NM_ERR_INVALID_JSON;

  gsize len = strlen(json);
  if (len > NATIVE_MESSAGING_MAX_SIZE) {
    return NM_ERR_MSG_TOO_LARGE;
  }

  /* Write 4-byte length prefix (little-endian) */
  guint8 len_buf[4];
  len_buf[0] = (guint8)(len & 0xFF);
  len_buf[1] = (guint8)((len >> 8) & 0xFF);
  len_buf[2] = (guint8)((len >> 16) & 0xFF);
  len_buf[3] = (guint8)((len >> 24) & 0xFF);

  if (!write_exact(len_buf, 4)) {
    return NM_ERR_IO;
  }

  if (!write_exact(json, len)) {
    return NM_ERR_IO;
  }

  /* Flush stdout */
  if (fflush(stdout) != 0) {
    return NM_ERR_IO;
  }

  return NM_OK;
}

NativeMessagingError native_messaging_write_response(const NativeMessagingResponse *resp) {
  if (!resp) return NM_ERR_INVALID_REQUEST;

  JsonBuilder *builder = json_builder_new();
  json_builder_begin_object(builder);

  /* Always include id if present */
  if (resp->id) {
    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, resp->id);
  }

  if (resp->success) {
    /* Success response */
    if (resp->result_json) {
      /* Parse and embed JSON result */
      JsonParser *parser = json_parser_new();
      if (json_parser_load_from_data(parser, resp->result_json, -1, NULL)) {
        json_builder_set_member_name(builder, "result");
        json_builder_add_value(builder, json_node_copy(json_parser_get_root(parser)));
      } else {
        /* Fallback: embed as string */
        json_builder_set_member_name(builder, "result");
        json_builder_add_string_value(builder, resp->result_json);
      }
      g_object_unref(parser);
    } else if (resp->result_str) {
      json_builder_set_member_name(builder, "result");
      json_builder_add_string_value(builder, resp->result_str);
    } else {
      json_builder_set_member_name(builder, "result");
      json_builder_add_null_value(builder);
    }
  } else {
    /* Error response */
    json_builder_set_member_name(builder, "error");
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "code");
    json_builder_add_int_value(builder, resp->error_code);

    json_builder_set_member_name(builder, "message");
    json_builder_add_string_value(builder,
      resp->error_message ? resp->error_message : native_messaging_error_message(resp->error_code));

    json_builder_end_object(builder);
  }

  json_builder_end_object(builder);

  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, json_builder_get_root(builder));
  gchar *json_str = json_generator_to_data(gen, NULL);

  g_object_unref(gen);
  g_object_unref(builder);

  NativeMessagingError rc = native_messaging_write_json(json_str);
  g_free(json_str);

  return rc;
}

/* Parse method string to enum */
static Nip07Method parse_method(const gchar *method) {
  if (!method) return NIP07_METHOD_UNKNOWN;

  if (g_strcmp0(method, "getPublicKey") == 0) {
    return NIP07_METHOD_GET_PUBLIC_KEY;
  } else if (g_strcmp0(method, "signEvent") == 0) {
    return NIP07_METHOD_SIGN_EVENT;
  } else if (g_strcmp0(method, "getRelays") == 0) {
    return NIP07_METHOD_GET_RELAYS;
  } else if (g_strcmp0(method, "nip04.encrypt") == 0) {
    return NIP07_METHOD_NIP04_ENCRYPT;
  } else if (g_strcmp0(method, "nip04.decrypt") == 0) {
    return NIP07_METHOD_NIP04_DECRYPT;
  } else if (g_strcmp0(method, "nip44.encrypt") == 0) {
    return NIP07_METHOD_NIP44_ENCRYPT;
  } else if (g_strcmp0(method, "nip44.decrypt") == 0) {
    return NIP07_METHOD_NIP44_DECRYPT;
  }

  return NIP07_METHOD_UNKNOWN;
}

NativeMessagingError native_messaging_parse_request(const gchar *json,
                                                    NativeMessagingRequest **out_req) {
  if (!json || !out_req) return NM_ERR_INVALID_REQUEST;
  *out_req = NULL;

  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, json, -1, &error)) {
    g_clear_error(&error);
    g_object_unref(parser);
    return NM_ERR_INVALID_JSON;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    g_object_unref(parser);
    return NM_ERR_INVALID_JSON;
  }

  JsonObject *obj = json_node_get_object(root);

  /* Required: method */
  if (!json_object_has_member(obj, "method")) {
    g_object_unref(parser);
    return NM_ERR_INVALID_REQUEST;
  }

  const gchar *method_str = json_object_get_string_member(obj, "method");
  Nip07Method method = parse_method(method_str);

  NativeMessagingRequest *req = g_new0(NativeMessagingRequest, 1);
  req->method = method;
  req->method_str = g_strdup(method_str);

  /* Optional: id */
  if (json_object_has_member(obj, "id")) {
    JsonNode *id_node = json_object_get_member(obj, "id");
    if (JSON_NODE_HOLDS_VALUE(id_node)) {
      if (json_node_get_value_type(id_node) == G_TYPE_STRING) {
        req->id = g_strdup(json_node_get_string(id_node));
      } else if (json_node_get_value_type(id_node) == G_TYPE_INT64) {
        req->id = g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(id_node));
      }
    }
  }

  /* Optional: origin */
  if (json_object_has_member(obj, "origin")) {
    req->origin = g_strdup(json_object_get_string_member(obj, "origin"));
  }

  /* Method-specific params */
  JsonObject *params = NULL;
  if (json_object_has_member(obj, "params")) {
    JsonNode *params_node = json_object_get_member(obj, "params");
    if (JSON_NODE_HOLDS_OBJECT(params_node)) {
      params = json_node_get_object(params_node);
    } else if (JSON_NODE_HOLDS_ARRAY(params_node)) {
      /* Some clients send params as array */
      JsonArray *arr = json_node_get_array(params_node);
      if (json_array_get_length(arr) > 0) {
        JsonNode *first = json_array_get_element(arr, 0);
        if (JSON_NODE_HOLDS_OBJECT(first)) {
          params = json_node_get_object(first);
        }
      }
    }
  }

  switch (method) {
    case NIP07_METHOD_SIGN_EVENT:
      /* For signEvent, params is the event object itself (or params.event) */
      if (params) {
        if (json_object_has_member(params, "event")) {
          JsonNode *event_node = json_object_get_member(params, "event");
          JsonGenerator *gen = json_generator_new();
          json_generator_set_root(gen, event_node);
          req->params.sign_event.event_json = json_generator_to_data(gen, NULL);
          g_object_unref(gen);
        } else {
          /* params IS the event */
          JsonGenerator *gen = json_generator_new();
          json_generator_set_root(gen, json_object_get_member(obj, "params"));
          req->params.sign_event.event_json = json_generator_to_data(gen, NULL);
          g_object_unref(gen);
        }
      }
      break;

    case NIP07_METHOD_NIP04_ENCRYPT:
    case NIP07_METHOD_NIP44_ENCRYPT:
      if (params) {
        if (json_object_has_member(params, "pubkey")) {
          req->params.encrypt.pubkey = g_strdup(json_object_get_string_member(params, "pubkey"));
        }
        if (json_object_has_member(params, "plaintext")) {
          req->params.encrypt.plaintext = g_strdup(json_object_get_string_member(params, "plaintext"));
        }
      }
      break;

    case NIP07_METHOD_NIP04_DECRYPT:
    case NIP07_METHOD_NIP44_DECRYPT:
      if (params) {
        if (json_object_has_member(params, "pubkey")) {
          req->params.decrypt.pubkey = g_strdup(json_object_get_string_member(params, "pubkey"));
        }
        if (json_object_has_member(params, "ciphertext")) {
          req->params.decrypt.ciphertext = g_strdup(json_object_get_string_member(params, "ciphertext"));
        }
      }
      break;

    default:
      break;
  }

  g_object_unref(parser);
  *out_req = req;
  return NM_OK;
}

void native_messaging_request_free(NativeMessagingRequest *req) {
  if (!req) return;

  g_free(req->id);
  g_free(req->method_str);
  g_free(req->origin);

  switch (req->method) {
    case NIP07_METHOD_SIGN_EVENT:
      g_free(req->params.sign_event.event_json);
      break;
    case NIP07_METHOD_NIP04_ENCRYPT:
    case NIP07_METHOD_NIP44_ENCRYPT:
      g_free(req->params.encrypt.pubkey);
      g_free(req->params.encrypt.plaintext);
      break;
    case NIP07_METHOD_NIP04_DECRYPT:
    case NIP07_METHOD_NIP44_DECRYPT:
      g_free(req->params.decrypt.pubkey);
      g_free(req->params.decrypt.ciphertext);
      break;
    default:
      break;
  }

  g_free(req);
}

void native_messaging_response_free(NativeMessagingResponse *resp) {
  if (!resp) return;
  g_free(resp->id);
  g_free(resp->result_str);
  g_free(resp->result_json);
  g_free(resp->error_message);
  g_free(resp);
}

NativeMessagingResponse *native_messaging_response_success(const gchar *id,
                                                           const gchar *result) {
  NativeMessagingResponse *resp = g_new0(NativeMessagingResponse, 1);
  resp->id = id ? g_strdup(id) : NULL;
  resp->success = TRUE;
  resp->result_str = result ? g_strdup(result) : NULL;
  return resp;
}

NativeMessagingResponse *native_messaging_response_success_json(const gchar *id,
                                                                const gchar *result_json) {
  NativeMessagingResponse *resp = g_new0(NativeMessagingResponse, 1);
  resp->id = id ? g_strdup(id) : NULL;
  resp->success = TRUE;
  resp->result_json = result_json ? g_strdup(result_json) : NULL;
  return resp;
}

NativeMessagingResponse *native_messaging_response_error(const gchar *id,
                                                         NativeMessagingError code,
                                                         const gchar *message) {
  NativeMessagingResponse *resp = g_new0(NativeMessagingResponse, 1);
  resp->id = id ? g_strdup(id) : NULL;
  resp->success = FALSE;
  resp->error_code = code;
  resp->error_message = message ? g_strdup(message) : NULL;
  return resp;
}

/* Check if event kind should be auto-approved */
static gboolean should_auto_approve(NativeMessagingContext *ctx, gint kind) {
  if (!ctx->auto_approve_kinds) return FALSE;

  for (guint i = 0; i < ctx->auto_approve_kinds->len; i++) {
    if (g_array_index(ctx->auto_approve_kinds, gint, i) == kind) {
      return TRUE;
    }
  }
  return FALSE;
}

/* Extract event kind from JSON */
static gint extract_event_kind(const gchar *event_json) {
  if (!event_json) return -1;

  JsonParser *parser = json_parser_new();
  if (!json_parser_load_from_data(parser, event_json, -1, NULL)) {
    g_object_unref(parser);
    return -1;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    g_object_unref(parser);
    return -1;
  }

  JsonObject *obj = json_node_get_object(root);
  gint kind = -1;
  if (json_object_has_member(obj, "kind")) {
    kind = (gint)json_object_get_int_member(obj, "kind");
  }

  g_object_unref(parser);
  return kind;
}

/* Generate preview text for approval dialog */
static gchar *generate_preview(const NativeMessagingRequest *req) {
  switch (req->method) {
    case NIP07_METHOD_GET_PUBLIC_KEY:
      return g_strdup("Share public key with application");

    case NIP07_METHOD_SIGN_EVENT: {
      gint kind = extract_event_kind(req->params.sign_event.event_json);
      if (kind >= 0) {
        return g_strdup_printf("Sign event (kind %d)", kind);
      }
      return g_strdup("Sign event");
    }

    case NIP07_METHOD_GET_RELAYS:
      return g_strdup("Share relay list with application");

    case NIP07_METHOD_NIP04_ENCRYPT:
    case NIP07_METHOD_NIP44_ENCRYPT:
      return g_strdup("Encrypt message");

    case NIP07_METHOD_NIP04_DECRYPT:
    case NIP07_METHOD_NIP44_DECRYPT:
      return g_strdup("Decrypt message");

    default:
      return g_strdup_printf("Unknown request: %s", req->method_str);
  }
}

/* Handle getPublicKey */
static NativeMessagingResponse *handle_get_public_key(NativeMessagingContext *ctx,
                                                      const NativeMessagingRequest *req) {
  gchar *npub = NULL;
  int rc = nostr_nip55l_get_public_key(&npub);

  if (rc != 0 || !npub) {
    return native_messaging_response_error(req->id, NM_ERR_NO_KEY,
                                           "No key available");
  }

  /* Convert npub to hex if needed */
  gchar *pubkey_hex = NULL;
  if (g_str_has_prefix(npub, "npub1")) {
    guint8 pk[32];
    if (nostr_nip19_decode_npub(npub, pk) == 0) {
      pubkey_hex = g_malloc(65);
      for (int i = 0; i < 32; i++) {
        g_snprintf(pubkey_hex + i*2, 3, "%02x", pk[i]);
      }
    }
  } else {
    pubkey_hex = g_strdup(npub);
  }

  free(npub);

  if (!pubkey_hex) {
    return native_messaging_response_error(req->id, NM_ERR_INTERNAL,
                                           "Failed to decode public key");
  }

  NativeMessagingResponse *resp = native_messaging_response_success(req->id, pubkey_hex);
  g_free(pubkey_hex);
  return resp;
}

/* Handle signEvent */
static NativeMessagingResponse *handle_sign_event(NativeMessagingContext *ctx,
                                                  const NativeMessagingRequest *req) {
  if (!req->params.sign_event.event_json) {
    return native_messaging_response_error(req->id, NM_ERR_INVALID_REQUEST,
                                           "Missing event parameter");
  }

  /* Check auto-approve */
  gint kind = extract_event_kind(req->params.sign_event.event_json);
  gboolean approved = should_auto_approve(ctx, kind);

  /* If not auto-approved, ask user */
  if (!approved && ctx->auth_cb) {
    gchar *preview = generate_preview(req);
    approved = ctx->auth_cb(req, preview, ctx->auth_cb_ud);
    g_free(preview);
  } else if (!ctx->auth_cb) {
    /* No auth callback, auto-approve all */
    approved = TRUE;
  }

  if (!approved) {
    return native_messaging_response_error(req->id, NM_ERR_USER_DENIED,
                                           "User denied signing request");
  }

  /* Sign the event */
  gchar *signature = NULL;
  int rc = nostr_nip55l_sign_event(req->params.sign_event.event_json,
                                   ctx->identity, NULL, &signature);

  if (rc != 0 || !signature) {
    return native_messaging_response_error(req->id, NM_ERR_SIGN_FAILED,
                                           "Signing failed");
  }

  /* Build signed event JSON
   * Parse original event, add id, pubkey, sig */
  JsonParser *parser = json_parser_new();
  if (!json_parser_load_from_data(parser, req->params.sign_event.event_json, -1, NULL)) {
    free(signature);
    g_object_unref(parser);
    return native_messaging_response_error(req->id, NM_ERR_INVALID_REQUEST,
                                           "Invalid event JSON");
  }

  JsonNode *root = json_parser_get_root(parser);
  JsonObject *event_obj = json_node_dup_object(root);
  g_object_unref(parser);

  /* Get public key */
  gchar *npub = NULL;
  nostr_nip55l_get_public_key(&npub);
  gchar *pubkey_hex = NULL;
  if (npub && g_str_has_prefix(npub, "npub1")) {
    guint8 pk[32];
    if (nostr_nip19_decode_npub(npub, pk) == 0) {
      pubkey_hex = g_malloc(65);
      for (int i = 0; i < 32; i++) {
        g_snprintf(pubkey_hex + i*2, 3, "%02x", pk[i]);
      }
    }
  }
  if (npub) free(npub);

  if (pubkey_hex) {
    json_object_set_string_member(event_obj, "pubkey", pubkey_hex);
    g_free(pubkey_hex);
  }

  /* Add signature */
  json_object_set_string_member(event_obj, "sig", signature);
  free(signature);

  /* Generate event ID (would need proper hashing - for now leave existing if present) */

  /* Serialize */
  JsonNode *result_node = json_node_new(JSON_NODE_OBJECT);
  json_node_set_object(result_node, event_obj);

  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, result_node);
  gchar *result_json = json_generator_to_data(gen, NULL);

  g_object_unref(gen);
  json_node_free(result_node);

  NativeMessagingResponse *resp = native_messaging_response_success_json(req->id, result_json);
  g_free(result_json);
  return resp;
}

/* Handle getRelays */
static NativeMessagingResponse *handle_get_relays(NativeMessagingContext *ctx,
                                                  const NativeMessagingRequest *req) {
  gchar *relays_json = NULL;
  int rc = nostr_nip55l_get_relays(&relays_json);

  if (rc != 0 || !relays_json) {
    /* Return empty object if no relays configured */
    return native_messaging_response_success_json(req->id, "{}");
  }

  NativeMessagingResponse *resp = native_messaging_response_success_json(req->id, relays_json);
  free(relays_json);
  return resp;
}

/* Handle nip04.encrypt */
static NativeMessagingResponse *handle_nip04_encrypt(NativeMessagingContext *ctx,
                                                     const NativeMessagingRequest *req) {
  if (!req->params.encrypt.pubkey || !req->params.encrypt.plaintext) {
    return native_messaging_response_error(req->id, NM_ERR_INVALID_REQUEST,
                                           "Missing pubkey or plaintext parameter");
  }

  gchar *ciphertext = NULL;
  int rc = nostr_nip55l_nip04_encrypt(req->params.encrypt.plaintext,
                                      req->params.encrypt.pubkey,
                                      ctx->identity,
                                      &ciphertext);

  if (rc != 0 || !ciphertext) {
    return native_messaging_response_error(req->id, NM_ERR_ENCRYPT_FAILED,
                                           "Encryption failed");
  }

  NativeMessagingResponse *resp = native_messaging_response_success(req->id, ciphertext);
  free(ciphertext);
  return resp;
}

/* Handle nip04.decrypt */
static NativeMessagingResponse *handle_nip04_decrypt(NativeMessagingContext *ctx,
                                                     const NativeMessagingRequest *req) {
  if (!req->params.decrypt.pubkey || !req->params.decrypt.ciphertext) {
    return native_messaging_response_error(req->id, NM_ERR_INVALID_REQUEST,
                                           "Missing pubkey or ciphertext parameter");
  }

  /* Check authorization for decrypt */
  gboolean approved = TRUE;
  if (ctx->auth_cb) {
    gchar *preview = generate_preview(req);
    approved = ctx->auth_cb(req, preview, ctx->auth_cb_ud);
    g_free(preview);
  }

  if (!approved) {
    return native_messaging_response_error(req->id, NM_ERR_USER_DENIED,
                                           "User denied decrypt request");
  }

  gchar *plaintext = NULL;
  int rc = nostr_nip55l_nip04_decrypt(req->params.decrypt.ciphertext,
                                      req->params.decrypt.pubkey,
                                      ctx->identity,
                                      &plaintext);

  if (rc != 0 || !plaintext) {
    return native_messaging_response_error(req->id, NM_ERR_DECRYPT_FAILED,
                                           "Decryption failed");
  }

  NativeMessagingResponse *resp = native_messaging_response_success(req->id, plaintext);
  free(plaintext);
  return resp;
}

/* Handle nip44.encrypt */
static NativeMessagingResponse *handle_nip44_encrypt(NativeMessagingContext *ctx,
                                                     const NativeMessagingRequest *req) {
  if (!req->params.encrypt.pubkey || !req->params.encrypt.plaintext) {
    return native_messaging_response_error(req->id, NM_ERR_INVALID_REQUEST,
                                           "Missing pubkey or plaintext parameter");
  }

  gchar *ciphertext = NULL;
  int rc = nostr_nip55l_nip44_encrypt(req->params.encrypt.plaintext,
                                      req->params.encrypt.pubkey,
                                      ctx->identity,
                                      &ciphertext);

  if (rc != 0 || !ciphertext) {
    return native_messaging_response_error(req->id, NM_ERR_ENCRYPT_FAILED,
                                           "NIP-44 encryption failed");
  }

  NativeMessagingResponse *resp = native_messaging_response_success(req->id, ciphertext);
  free(ciphertext);
  return resp;
}

/* Handle nip44.decrypt */
static NativeMessagingResponse *handle_nip44_decrypt(NativeMessagingContext *ctx,
                                                     const NativeMessagingRequest *req) {
  if (!req->params.decrypt.pubkey || !req->params.decrypt.ciphertext) {
    return native_messaging_response_error(req->id, NM_ERR_INVALID_REQUEST,
                                           "Missing pubkey or ciphertext parameter");
  }

  /* Check authorization for decrypt */
  gboolean approved = TRUE;
  if (ctx->auth_cb) {
    gchar *preview = generate_preview(req);
    approved = ctx->auth_cb(req, preview, ctx->auth_cb_ud);
    g_free(preview);
  }

  if (!approved) {
    return native_messaging_response_error(req->id, NM_ERR_USER_DENIED,
                                           "User denied decrypt request");
  }

  gchar *plaintext = NULL;
  int rc = nostr_nip55l_nip44_decrypt(req->params.decrypt.ciphertext,
                                      req->params.decrypt.pubkey,
                                      ctx->identity,
                                      &plaintext);

  if (rc != 0 || !plaintext) {
    return native_messaging_response_error(req->id, NM_ERR_DECRYPT_FAILED,
                                           "NIP-44 decryption failed");
  }

  NativeMessagingResponse *resp = native_messaging_response_success(req->id, plaintext);
  free(plaintext);
  return resp;
}

NativeMessagingError native_messaging_process_request(NativeMessagingContext *ctx,
                                                      const NativeMessagingRequest *req,
                                                      NativeMessagingResponse **out_resp) {
  if (!ctx || !req || !out_resp) return NM_ERR_INVALID_REQUEST;

  *out_resp = NULL;

  switch (req->method) {
    case NIP07_METHOD_GET_PUBLIC_KEY:
      *out_resp = handle_get_public_key(ctx, req);
      break;

    case NIP07_METHOD_SIGN_EVENT:
      *out_resp = handle_sign_event(ctx, req);
      break;

    case NIP07_METHOD_GET_RELAYS:
      *out_resp = handle_get_relays(ctx, req);
      break;

    case NIP07_METHOD_NIP04_ENCRYPT:
      *out_resp = handle_nip04_encrypt(ctx, req);
      break;

    case NIP07_METHOD_NIP04_DECRYPT:
      *out_resp = handle_nip04_decrypt(ctx, req);
      break;

    case NIP07_METHOD_NIP44_ENCRYPT:
      *out_resp = handle_nip44_encrypt(ctx, req);
      break;

    case NIP07_METHOD_NIP44_DECRYPT:
      *out_resp = handle_nip44_decrypt(ctx, req);
      break;

    default:
      *out_resp = native_messaging_response_error(req->id, NM_ERR_UNKNOWN_METHOD,
                                                  "Unknown method");
      break;
  }

  return NM_OK;
}

NativeMessagingError native_messaging_run(NativeMessagingContext *ctx) {
  if (!ctx) return NM_ERR_INVALID_REQUEST;

  while (TRUE) {
    NativeMessagingError read_err = NM_OK;
    gchar *msg = native_messaging_read_message(&read_err);

    if (!msg) {
      if (read_err == NM_ERR_IO) {
        /* EOF or read error - clean shutdown */
        return NM_OK;
      }
      /* Write error response for other errors */
      NativeMessagingResponse *resp = native_messaging_response_error(NULL, read_err, NULL);
      native_messaging_write_response(resp);
      native_messaging_response_free(resp);
      continue;
    }

    /* Parse request */
    NativeMessagingRequest *req = NULL;
    NativeMessagingError parse_err = native_messaging_parse_request(msg, &req);
    g_free(msg);

    if (parse_err != NM_OK) {
      NativeMessagingResponse *resp = native_messaging_response_error(NULL, parse_err, NULL);
      native_messaging_write_response(resp);
      native_messaging_response_free(resp);
      continue;
    }

    /* Process request */
    NativeMessagingResponse *resp = NULL;
    native_messaging_process_request(ctx, req, &resp);

    if (resp) {
      native_messaging_write_response(resp);
      native_messaging_response_free(resp);
    }

    native_messaging_request_free(req);
  }

  return NM_OK;
}
