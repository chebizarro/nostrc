/**
 * NIP-07 Browser Extension Interface - Implementation
 *
 * This implements a D-Bus client that communicates with a NIP-07 compatible
 * signer service, providing the same semantics as window.nostr in browsers.
 */

#include "nip07_extension.h"
#include <string.h>
#include <json-glib/json-glib.h>

/* D-Bus method names matching NIP-07 API */
#define METHOD_GET_PUBLIC_KEY  "GetPublicKey"
#define METHOD_SIGN_EVENT      "SignEvent"
#define METHOD_GET_RELAYS      "GetRelays"
#define METHOD_NIP04_ENCRYPT   "Nip04Encrypt"
#define METHOD_NIP04_DECRYPT   "Nip04Decrypt"
#define METHOD_NIP44_ENCRYPT   "Nip44Encrypt"
#define METHOD_NIP44_DECRYPT   "Nip44Decrypt"

/* D-Bus timeout in milliseconds */
#define DBUS_TIMEOUT_MS  30000

G_DEFINE_QUARK(gnostr-nip07-error-quark, gnostr_nip07_error)

const char *gnostr_nip07_request_to_string(GnostrNip07Request request) {
  switch (request) {
    case GNOSTR_NIP07_GET_PUBLIC_KEY:
      return "getPublicKey";
    case GNOSTR_NIP07_SIGN_EVENT:
      return "signEvent";
    case GNOSTR_NIP07_GET_RELAYS:
      return "getRelays";
    case GNOSTR_NIP07_NIP04_ENCRYPT:
      return "nip04.encrypt";
    case GNOSTR_NIP07_NIP04_DECRYPT:
      return "nip04.decrypt";
    case GNOSTR_NIP07_NIP44_ENCRYPT:
      return "nip44.encrypt";
    case GNOSTR_NIP07_NIP44_DECRYPT:
      return "nip44.decrypt";
    default:
      return "unknown";
  }
}

void gnostr_nip07_response_free(GnostrNip07Response *response) {
  if (!response) return;
  g_free(response->result_str);
  g_free(response->error_msg);
  g_free(response);
}

void gnostr_nip07_relay_free(GnostrNip07Relay *relay) {
  if (!relay) return;
  g_free(relay->url);
  g_free(relay);
}

/* ---- D-Bus Connection Helper ---- */

/**
 * Get a connection to the session bus.
 * Caller owns the returned connection.
 */
static GDBusConnection *get_session_bus(GError **error) {
  GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, error);
  if (!conn) {
    g_debug("nip07: Failed to connect to session bus");
  }
  return conn;
}

/**
 * Create a response from D-Bus reply.
 */
static GnostrNip07Response *create_response_from_variant(GVariant *result,
                                                          gboolean is_string_result) {
  GnostrNip07Response *response = g_new0(GnostrNip07Response, 1);

  if (!result) {
    response->success = FALSE;
    response->error_msg = g_strdup("No result from D-Bus call");
    return response;
  }

  if (is_string_result) {
    /* Expect (s) or (ss) - result string, optional error */
    const char *result_str = NULL;
    const char *error_str = NULL;

    if (g_variant_is_of_type(result, G_VARIANT_TYPE("(s)"))) {
      g_variant_get(result, "(&s)", &result_str);
    } else if (g_variant_is_of_type(result, G_VARIANT_TYPE("(ss)"))) {
      g_variant_get(result, "(&s&s)", &result_str, &error_str);
    } else if (g_variant_is_of_type(result, G_VARIANT_TYPE("(bs)"))) {
      gboolean ok;
      g_variant_get(result, "(b&s)", &ok, &result_str);
      if (!ok) {
        error_str = result_str;
        result_str = NULL;
      }
    }

    if (error_str && *error_str) {
      response->success = FALSE;
      response->error_msg = g_strdup(error_str);
    } else if (result_str && *result_str) {
      response->success = TRUE;
      response->result_str = g_strdup(result_str);
    } else {
      response->success = FALSE;
      response->error_msg = g_strdup("Empty result from signer");
    }
  } else {
    /* Non-string result - just check for error */
    response->success = TRUE;
  }

  return response;
}

/* ---- Synchronous D-Bus Calls ---- */

gboolean gnostr_nip07_service_available(void) {
  GError *error = NULL;
  GDBusConnection *conn = get_session_bus(&error);

  if (!conn) {
    if (error) g_error_free(error);
    return FALSE;
  }

  /* Check if the NIP-07 service name is registered */
  GVariant *result = g_dbus_connection_call_sync(
      conn,
      "org.freedesktop.DBus",
      "/org/freedesktop/DBus",
      "org.freedesktop.DBus",
      "NameHasOwner",
      g_variant_new("(s)", GNOSTR_NIP07_DBUS_NAME),
      G_VARIANT_TYPE("(b)"),
      G_DBUS_CALL_FLAGS_NONE,
      5000,  /* 5 second timeout */
      NULL,
      &error);

  g_object_unref(conn);

  if (!result) {
    if (error) {
      g_debug("nip07: NameHasOwner check failed: %s", error->message);
      g_error_free(error);
    }
    return FALSE;
  }

  gboolean has_owner = FALSE;
  g_variant_get(result, "(b)", &has_owner);
  g_variant_unref(result);

  g_debug("nip07: Service %s available: %s", GNOSTR_NIP07_DBUS_NAME,
          has_owner ? "yes" : "no");

  return has_owner;
}

GnostrNip07Response *gnostr_nip07_get_public_key(GError **error) {
  GDBusConnection *conn = get_session_bus(error);
  if (!conn) {
    return NULL;
  }

  GError *call_error = NULL;
  GVariant *result = g_dbus_connection_call_sync(
      conn,
      GNOSTR_NIP07_DBUS_NAME,
      GNOSTR_NIP07_DBUS_PATH,
      GNOSTR_NIP07_DBUS_INTERFACE,
      METHOD_GET_PUBLIC_KEY,
      NULL,  /* No parameters */
      G_VARIANT_TYPE("(s)"),
      G_DBUS_CALL_FLAGS_NONE,
      DBUS_TIMEOUT_MS,
      NULL,
      &call_error);

  g_object_unref(conn);

  if (call_error) {
    g_debug("nip07: GetPublicKey failed: %s", call_error->message);
    if (error) {
      *error = g_error_new(GNOSTR_NIP07_ERROR,
                           GNOSTR_NIP07_ERROR_FAILED,
                           "D-Bus call failed: %s", call_error->message);
    }
    g_error_free(call_error);
    return NULL;
  }

  GnostrNip07Response *response = create_response_from_variant(result, TRUE);
  g_variant_unref(result);

  g_debug("nip07: GetPublicKey %s: %s",
          response->success ? "succeeded" : "failed",
          response->success ? response->result_str : response->error_msg);

  return response;
}

GnostrNip07Response *gnostr_nip07_sign_event(const char *unsigned_event_json,
                                              GError **error) {
  if (!unsigned_event_json || !*unsigned_event_json) {
    if (error) {
      *error = g_error_new(GNOSTR_NIP07_ERROR,
                           GNOSTR_NIP07_ERROR_INVALID_EVENT,
                           "Event JSON is empty");
    }
    return NULL;
  }

  /* Validate JSON before sending */
  JsonParser *parser = json_parser_new();
  GError *parse_error = NULL;
  if (!json_parser_load_from_data(parser, unsigned_event_json, -1, &parse_error)) {
    g_object_unref(parser);
    if (error) {
      *error = g_error_new(GNOSTR_NIP07_ERROR,
                           GNOSTR_NIP07_ERROR_INVALID_EVENT,
                           "Invalid event JSON: %s", parse_error->message);
    }
    g_error_free(parse_error);
    return NULL;
  }
  g_object_unref(parser);

  GDBusConnection *conn = get_session_bus(error);
  if (!conn) {
    return NULL;
  }

  GError *call_error = NULL;
  GVariant *result = g_dbus_connection_call_sync(
      conn,
      GNOSTR_NIP07_DBUS_NAME,
      GNOSTR_NIP07_DBUS_PATH,
      GNOSTR_NIP07_DBUS_INTERFACE,
      METHOD_SIGN_EVENT,
      g_variant_new("(s)", unsigned_event_json),
      G_VARIANT_TYPE("(s)"),
      G_DBUS_CALL_FLAGS_NONE,
      DBUS_TIMEOUT_MS,
      NULL,
      &call_error);

  g_object_unref(conn);

  if (call_error) {
    g_debug("nip07: SignEvent failed: %s", call_error->message);
    if (error) {
      *error = g_error_new(GNOSTR_NIP07_ERROR,
                           GNOSTR_NIP07_ERROR_FAILED,
                           "D-Bus call failed: %s", call_error->message);
    }
    g_error_free(call_error);
    return NULL;
  }

  GnostrNip07Response *response = create_response_from_variant(result, TRUE);
  g_variant_unref(result);

  g_debug("nip07: SignEvent %s", response->success ? "succeeded" : "failed");

  return response;
}

GnostrNip07Response *gnostr_nip07_get_relays(GError **error) {
  GDBusConnection *conn = get_session_bus(error);
  if (!conn) {
    return NULL;
  }

  GError *call_error = NULL;
  GVariant *result = g_dbus_connection_call_sync(
      conn,
      GNOSTR_NIP07_DBUS_NAME,
      GNOSTR_NIP07_DBUS_PATH,
      GNOSTR_NIP07_DBUS_INTERFACE,
      METHOD_GET_RELAYS,
      NULL,  /* No parameters */
      G_VARIANT_TYPE("(s)"),
      G_DBUS_CALL_FLAGS_NONE,
      DBUS_TIMEOUT_MS,
      NULL,
      &call_error);

  g_object_unref(conn);

  if (call_error) {
    g_debug("nip07: GetRelays failed: %s", call_error->message);
    if (error) {
      *error = g_error_new(GNOSTR_NIP07_ERROR,
                           GNOSTR_NIP07_ERROR_FAILED,
                           "D-Bus call failed: %s", call_error->message);
    }
    g_error_free(call_error);
    return NULL;
  }

  GnostrNip07Response *response = create_response_from_variant(result, TRUE);
  g_variant_unref(result);

  g_debug("nip07: GetRelays %s", response->success ? "succeeded" : "failed");

  return response;
}

GnostrNip07Response *gnostr_nip07_encrypt(const char *recipient_pubkey,
                                           const char *plaintext,
                                           gboolean use_nip44,
                                           GError **error) {
  if (!recipient_pubkey || strlen(recipient_pubkey) != 64) {
    if (error) {
      *error = g_error_new(GNOSTR_NIP07_ERROR,
                           GNOSTR_NIP07_ERROR_INVALID_PUBKEY,
                           "Invalid recipient pubkey (expected 64 hex chars)");
    }
    return NULL;
  }

  if (!plaintext) {
    if (error) {
      *error = g_error_new(GNOSTR_NIP07_ERROR,
                           GNOSTR_NIP07_ERROR_FAILED,
                           "Plaintext is NULL");
    }
    return NULL;
  }

  GDBusConnection *conn = get_session_bus(error);
  if (!conn) {
    return NULL;
  }

  const char *method = use_nip44 ? METHOD_NIP44_ENCRYPT : METHOD_NIP04_ENCRYPT;

  GError *call_error = NULL;
  GVariant *result = g_dbus_connection_call_sync(
      conn,
      GNOSTR_NIP07_DBUS_NAME,
      GNOSTR_NIP07_DBUS_PATH,
      GNOSTR_NIP07_DBUS_INTERFACE,
      method,
      g_variant_new("(ss)", recipient_pubkey, plaintext),
      G_VARIANT_TYPE("(s)"),
      G_DBUS_CALL_FLAGS_NONE,
      DBUS_TIMEOUT_MS,
      NULL,
      &call_error);

  g_object_unref(conn);

  if (call_error) {
    g_debug("nip07: %s failed: %s", method, call_error->message);
    if (error) {
      *error = g_error_new(GNOSTR_NIP07_ERROR,
                           GNOSTR_NIP07_ERROR_ENCRYPTION_FAILED,
                           "D-Bus call failed: %s", call_error->message);
    }
    g_error_free(call_error);
    return NULL;
  }

  GnostrNip07Response *response = create_response_from_variant(result, TRUE);
  g_variant_unref(result);

  g_debug("nip07: %s %s", method, response->success ? "succeeded" : "failed");

  return response;
}

GnostrNip07Response *gnostr_nip07_decrypt(const char *sender_pubkey,
                                           const char *ciphertext,
                                           gboolean use_nip44,
                                           GError **error) {
  if (!sender_pubkey || strlen(sender_pubkey) != 64) {
    if (error) {
      *error = g_error_new(GNOSTR_NIP07_ERROR,
                           GNOSTR_NIP07_ERROR_INVALID_PUBKEY,
                           "Invalid sender pubkey (expected 64 hex chars)");
    }
    return NULL;
  }

  if (!ciphertext || !*ciphertext) {
    if (error) {
      *error = g_error_new(GNOSTR_NIP07_ERROR,
                           GNOSTR_NIP07_ERROR_FAILED,
                           "Ciphertext is empty");
    }
    return NULL;
  }

  GDBusConnection *conn = get_session_bus(error);
  if (!conn) {
    return NULL;
  }

  const char *method = use_nip44 ? METHOD_NIP44_DECRYPT : METHOD_NIP04_DECRYPT;

  GError *call_error = NULL;
  GVariant *result = g_dbus_connection_call_sync(
      conn,
      GNOSTR_NIP07_DBUS_NAME,
      GNOSTR_NIP07_DBUS_PATH,
      GNOSTR_NIP07_DBUS_INTERFACE,
      method,
      g_variant_new("(ss)", sender_pubkey, ciphertext),
      G_VARIANT_TYPE("(s)"),
      G_DBUS_CALL_FLAGS_NONE,
      DBUS_TIMEOUT_MS,
      NULL,
      &call_error);

  g_object_unref(conn);

  if (call_error) {
    g_debug("nip07: %s failed: %s", method, call_error->message);
    if (error) {
      *error = g_error_new(GNOSTR_NIP07_ERROR,
                           GNOSTR_NIP07_ERROR_ENCRYPTION_FAILED,
                           "D-Bus call failed: %s", call_error->message);
    }
    g_error_free(call_error);
    return NULL;
  }

  GnostrNip07Response *response = create_response_from_variant(result, TRUE);
  g_variant_unref(result);

  g_debug("nip07: %s %s", method, response->success ? "succeeded" : "failed");

  return response;
}

/* ---- Asynchronous D-Bus Calls ---- */

typedef struct {
  GnostrNip07Request request_type;
  char *unsigned_event_json;  /* For sign_event */
  char *pubkey;               /* For encrypt/decrypt */
  char *text;                 /* plaintext or ciphertext */
  gboolean use_nip44;         /* For encrypt/decrypt */
} AsyncCallData;

static void async_call_data_free(AsyncCallData *data) {
  if (!data) return;
  g_free(data->unsigned_event_json);
  g_free(data->pubkey);
  g_free(data->text);
  g_free(data);
}

static void on_dbus_call_complete(GObject *source, GAsyncResult *res, gpointer user_data) {
  GTask *task = G_TASK(user_data);
  GDBusConnection *conn = G_DBUS_CONNECTION(source);
  AsyncCallData *data = g_task_get_task_data(task);
  (void)data;

  GError *error = NULL;
  GVariant *result = g_dbus_connection_call_finish(conn, res, &error);

  if (error) {
    g_task_return_error(task, error);
  } else {
    GnostrNip07Response *response = create_response_from_variant(result, TRUE);
    g_variant_unref(result);
    g_task_return_pointer(task, response, (GDestroyNotify)gnostr_nip07_response_free);
  }

  g_object_unref(conn);
  g_object_unref(task);
}

void gnostr_nip07_get_public_key_async(GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data) {
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);

  GError *error = NULL;
  GDBusConnection *conn = get_session_bus(&error);
  if (!conn) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }

  AsyncCallData *data = g_new0(AsyncCallData, 1);
  data->request_type = GNOSTR_NIP07_GET_PUBLIC_KEY;
  g_task_set_task_data(task, data, (GDestroyNotify)async_call_data_free);

  g_dbus_connection_call(
      conn,
      GNOSTR_NIP07_DBUS_NAME,
      GNOSTR_NIP07_DBUS_PATH,
      GNOSTR_NIP07_DBUS_INTERFACE,
      METHOD_GET_PUBLIC_KEY,
      NULL,
      G_VARIANT_TYPE("(s)"),
      G_DBUS_CALL_FLAGS_NONE,
      DBUS_TIMEOUT_MS,
      cancellable,
      on_dbus_call_complete,
      task);
}

GnostrNip07Response *gnostr_nip07_get_public_key_finish(GAsyncResult *result,
                                                         GError **error) {
  g_return_val_if_fail(G_IS_TASK(result), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

void gnostr_nip07_sign_event_async(const char *unsigned_event_json,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data) {
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);

  if (!unsigned_event_json || !*unsigned_event_json) {
    g_task_return_new_error(task, GNOSTR_NIP07_ERROR,
                            GNOSTR_NIP07_ERROR_INVALID_EVENT,
                            "Event JSON is empty");
    g_object_unref(task);
    return;
  }

  GError *error = NULL;
  GDBusConnection *conn = get_session_bus(&error);
  if (!conn) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }

  AsyncCallData *data = g_new0(AsyncCallData, 1);
  data->request_type = GNOSTR_NIP07_SIGN_EVENT;
  data->unsigned_event_json = g_strdup(unsigned_event_json);
  g_task_set_task_data(task, data, (GDestroyNotify)async_call_data_free);

  g_dbus_connection_call(
      conn,
      GNOSTR_NIP07_DBUS_NAME,
      GNOSTR_NIP07_DBUS_PATH,
      GNOSTR_NIP07_DBUS_INTERFACE,
      METHOD_SIGN_EVENT,
      g_variant_new("(s)", unsigned_event_json),
      G_VARIANT_TYPE("(s)"),
      G_DBUS_CALL_FLAGS_NONE,
      DBUS_TIMEOUT_MS,
      cancellable,
      on_dbus_call_complete,
      task);
}

GnostrNip07Response *gnostr_nip07_sign_event_finish(GAsyncResult *result,
                                                     GError **error) {
  g_return_val_if_fail(G_IS_TASK(result), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

void gnostr_nip07_get_relays_async(GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data) {
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);

  GError *error = NULL;
  GDBusConnection *conn = get_session_bus(&error);
  if (!conn) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }

  AsyncCallData *data = g_new0(AsyncCallData, 1);
  data->request_type = GNOSTR_NIP07_GET_RELAYS;
  g_task_set_task_data(task, data, (GDestroyNotify)async_call_data_free);

  g_dbus_connection_call(
      conn,
      GNOSTR_NIP07_DBUS_NAME,
      GNOSTR_NIP07_DBUS_PATH,
      GNOSTR_NIP07_DBUS_INTERFACE,
      METHOD_GET_RELAYS,
      NULL,
      G_VARIANT_TYPE("(s)"),
      G_DBUS_CALL_FLAGS_NONE,
      DBUS_TIMEOUT_MS,
      cancellable,
      on_dbus_call_complete,
      task);
}

GnostrNip07Response *gnostr_nip07_get_relays_finish(GAsyncResult *result,
                                                     GError **error) {
  g_return_val_if_fail(G_IS_TASK(result), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

void gnostr_nip07_encrypt_async(const char *recipient_pubkey,
                                 const char *plaintext,
                                 gboolean use_nip44,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data) {
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);

  if (!recipient_pubkey || strlen(recipient_pubkey) != 64) {
    g_task_return_new_error(task, GNOSTR_NIP07_ERROR,
                            GNOSTR_NIP07_ERROR_INVALID_PUBKEY,
                            "Invalid recipient pubkey");
    g_object_unref(task);
    return;
  }

  GError *error = NULL;
  GDBusConnection *conn = get_session_bus(&error);
  if (!conn) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }

  AsyncCallData *data = g_new0(AsyncCallData, 1);
  data->request_type = use_nip44 ? GNOSTR_NIP07_NIP44_ENCRYPT : GNOSTR_NIP07_NIP04_ENCRYPT;
  data->pubkey = g_strdup(recipient_pubkey);
  data->text = g_strdup(plaintext);
  data->use_nip44 = use_nip44;
  g_task_set_task_data(task, data, (GDestroyNotify)async_call_data_free);

  const char *method = use_nip44 ? METHOD_NIP44_ENCRYPT : METHOD_NIP04_ENCRYPT;

  g_dbus_connection_call(
      conn,
      GNOSTR_NIP07_DBUS_NAME,
      GNOSTR_NIP07_DBUS_PATH,
      GNOSTR_NIP07_DBUS_INTERFACE,
      method,
      g_variant_new("(ss)", recipient_pubkey, plaintext ? plaintext : ""),
      G_VARIANT_TYPE("(s)"),
      G_DBUS_CALL_FLAGS_NONE,
      DBUS_TIMEOUT_MS,
      cancellable,
      on_dbus_call_complete,
      task);
}

GnostrNip07Response *gnostr_nip07_encrypt_finish(GAsyncResult *result,
                                                  GError **error) {
  g_return_val_if_fail(G_IS_TASK(result), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

void gnostr_nip07_decrypt_async(const char *sender_pubkey,
                                 const char *ciphertext,
                                 gboolean use_nip44,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data) {
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);

  if (!sender_pubkey || strlen(sender_pubkey) != 64) {
    g_task_return_new_error(task, GNOSTR_NIP07_ERROR,
                            GNOSTR_NIP07_ERROR_INVALID_PUBKEY,
                            "Invalid sender pubkey");
    g_object_unref(task);
    return;
  }

  if (!ciphertext || !*ciphertext) {
    g_task_return_new_error(task, GNOSTR_NIP07_ERROR,
                            GNOSTR_NIP07_ERROR_FAILED,
                            "Ciphertext is empty");
    g_object_unref(task);
    return;
  }

  GError *error = NULL;
  GDBusConnection *conn = get_session_bus(&error);
  if (!conn) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }

  AsyncCallData *data = g_new0(AsyncCallData, 1);
  data->request_type = use_nip44 ? GNOSTR_NIP07_NIP44_DECRYPT : GNOSTR_NIP07_NIP04_DECRYPT;
  data->pubkey = g_strdup(sender_pubkey);
  data->text = g_strdup(ciphertext);
  data->use_nip44 = use_nip44;
  g_task_set_task_data(task, data, (GDestroyNotify)async_call_data_free);

  const char *method = use_nip44 ? METHOD_NIP44_DECRYPT : METHOD_NIP04_DECRYPT;

  g_dbus_connection_call(
      conn,
      GNOSTR_NIP07_DBUS_NAME,
      GNOSTR_NIP07_DBUS_PATH,
      GNOSTR_NIP07_DBUS_INTERFACE,
      method,
      g_variant_new("(ss)", sender_pubkey, ciphertext),
      G_VARIANT_TYPE("(s)"),
      G_DBUS_CALL_FLAGS_NONE,
      DBUS_TIMEOUT_MS,
      cancellable,
      on_dbus_call_complete,
      task);
}

GnostrNip07Response *gnostr_nip07_decrypt_finish(GAsyncResult *result,
                                                  GError **error) {
  g_return_val_if_fail(G_IS_TASK(result), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

/* ---- Utility Functions ---- */

char *gnostr_nip07_format_unsigned_event(gint kind,
                                          const char *content,
                                          const char *tags_json,
                                          gint64 created_at) {
  JsonBuilder *builder = json_builder_new();

  json_builder_begin_object(builder);

  /* kind */
  json_builder_set_member_name(builder, "kind");
  json_builder_add_int_value(builder, kind);

  /* content */
  json_builder_set_member_name(builder, "content");
  json_builder_add_string_value(builder, content ? content : "");

  /* created_at */
  json_builder_set_member_name(builder, "created_at");
  if (created_at > 0) {
    json_builder_add_int_value(builder, created_at);
  } else {
    json_builder_add_int_value(builder, g_get_real_time() / 1000000);
  }

  /* tags */
  json_builder_set_member_name(builder, "tags");
  if (tags_json && *tags_json) {
    /* Parse provided tags JSON */
    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    if (json_parser_load_from_data(parser, tags_json, -1, &error)) {
      JsonNode *tags_node = json_parser_get_root(parser);
      if (tags_node && JSON_NODE_HOLDS_ARRAY(tags_node)) {
        json_builder_add_value(builder, json_node_copy(tags_node));
      } else {
        /* Invalid tags, use empty array */
        json_builder_begin_array(builder);
        json_builder_end_array(builder);
      }
    } else {
      g_warning("nip07: Failed to parse tags JSON: %s", error->message);
      g_error_free(error);
      json_builder_begin_array(builder);
      json_builder_end_array(builder);
    }
    g_object_unref(parser);
  } else {
    /* Empty tags array */
    json_builder_begin_array(builder);
    json_builder_end_array(builder);
  }

  json_builder_end_object(builder);

  /* Generate JSON string */
  JsonGenerator *generator = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(generator, root);

  char *result = json_generator_to_data(generator, NULL);

  json_node_unref(root);
  g_object_unref(generator);
  g_object_unref(builder);

  return result;
}

gboolean gnostr_nip07_parse_signed_event(const char *signed_event_json,
                                          char **out_id,
                                          char **out_pubkey,
                                          char **out_sig,
                                          gint *out_kind,
                                          gint64 *out_created_at) {
  if (!signed_event_json || !*signed_event_json) {
    return FALSE;
  }

  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, signed_event_json, -1, &error)) {
    g_warning("nip07: Failed to parse signed event: %s", error->message);
    g_error_free(error);
    g_object_unref(parser);
    return FALSE;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_object_unref(parser);
    return FALSE;
  }

  JsonObject *obj = json_node_get_object(root);

  if (out_id) {
    const char *id = json_object_get_string_member_with_default(obj, "id", NULL);
    *out_id = id ? g_strdup(id) : NULL;
  }

  if (out_pubkey) {
    const char *pubkey = json_object_get_string_member_with_default(obj, "pubkey", NULL);
    *out_pubkey = pubkey ? g_strdup(pubkey) : NULL;
  }

  if (out_sig) {
    const char *sig = json_object_get_string_member_with_default(obj, "sig", NULL);
    *out_sig = sig ? g_strdup(sig) : NULL;
  }

  if (out_kind) {
    *out_kind = (gint)json_object_get_int_member_with_default(obj, "kind", 0);
  }

  if (out_created_at) {
    *out_created_at = json_object_get_int_member_with_default(obj, "created_at", 0);
  }

  g_object_unref(parser);
  return TRUE;
}

GList *gnostr_nip07_parse_relays(const char *relays_json) {
  if (!relays_json || !*relays_json) {
    return NULL;
  }

  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, relays_json, -1, &error)) {
    g_warning("nip07: Failed to parse relays JSON: %s", error->message);
    g_error_free(error);
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_object_unref(parser);
    return NULL;
  }

  JsonObject *obj = json_node_get_object(root);
  GList *relays = NULL;

  /* Format: {"wss://relay.example": {"read": true, "write": true}, ...} */
  GList *members = json_object_get_members(obj);
  for (GList *l = members; l; l = l->next) {
    const char *url = (const char *)l->data;
    JsonNode *value_node = json_object_get_member(obj, url);

    if (value_node && JSON_NODE_HOLDS_OBJECT(value_node)) {
      JsonObject *relay_obj = json_node_get_object(value_node);

      GnostrNip07Relay *relay = g_new0(GnostrNip07Relay, 1);
      relay->url = g_strdup(url);
      relay->read = json_object_get_boolean_member_with_default(relay_obj, "read", TRUE);
      relay->write = json_object_get_boolean_member_with_default(relay_obj, "write", TRUE);

      relays = g_list_prepend(relays, relay);
    }
  }
  g_list_free(members);

  g_object_unref(parser);

  /* Reverse to maintain original order */
  return g_list_reverse(relays);
}
