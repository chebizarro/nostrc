/**
 * NIP-55 Android Signer Application Support - Implementation
 *
 * This implements the CLIENT-side protocol for communicating with
 * Android signer applications via intent URIs.
 */

#include "nip55_android.h"
#include <string.h>
#include <json.h>

/* Intent URI components */
#define INTENT_PREFIX "intent://"
#define INTENT_SUFFIX "#Intent;scheme=nostrsigner;package=%s;end"
#define INTENT_SUFFIX_WITH_CALLBACK "#Intent;scheme=nostrsigner;package=%s;S.callbackUrl=%s;end"

/* Default signer package (Amber) */
#define DEFAULT_SIGNER_PACKAGE GNOSTR_ANDROID_SIGNER_AMBER

/* NIP-55 action paths */
#define ACTION_SIGN "sign"
#define ACTION_ENCRYPT "encrypt"
#define ACTION_DECRYPT "decrypt"
#define ACTION_GET_PUBLIC_KEY "get_public_key"

/* Parameter names */
#define PARAM_EVENT "event"
#define PARAM_PLAINTEXT "plaintext"
#define PARAM_CIPHERTEXT "ciphertext"
#define PARAM_PUBKEY "pubKey"
#define PARAM_TYPE "type"

/* Encryption type strings */
#define ENCRYPTION_TYPE_NIP04 "nip04"
#define ENCRYPTION_TYPE_NIP44 "nip44"

const char *gnostr_android_signer_request_to_string(GnostrAndroidSignerRequest request) {
  switch (request) {
    case GNOSTR_ANDROID_SIGNER_SIGN:
      return "sign";
    case GNOSTR_ANDROID_SIGNER_ENCRYPT:
      return "encrypt";
    case GNOSTR_ANDROID_SIGNER_DECRYPT:
      return "decrypt";
    case GNOSTR_ANDROID_SIGNER_GET_PUBLIC_KEY:
      return "get_public_key";
    default:
      return "unknown";
  }
}

const char *gnostr_android_encryption_type_to_string(GnostrAndroidEncryptionType type) {
  switch (type) {
    case GNOSTR_ANDROID_ENCRYPTION_NIP04:
      return ENCRYPTION_TYPE_NIP04;
    case GNOSTR_ANDROID_ENCRYPTION_NIP44:
      return ENCRYPTION_TYPE_NIP44;
    default:
      return "unknown";
  }
}

gboolean gnostr_android_signer_available(void) {
  /* Check for Android environment indicators */

  /* ANDROID_ROOT is set on Android systems */
  const char *android_root = g_getenv("ANDROID_ROOT");
  if (android_root && *android_root) {
    g_debug("nip55: Android detected via ANDROID_ROOT=%s", android_root);
    return TRUE;
  }

  /* ANDROID_DATA is another indicator */
  const char *android_data = g_getenv("ANDROID_DATA");
  if (android_data && *android_data) {
    g_debug("nip55: Android detected via ANDROID_DATA=%s", android_data);
    return TRUE;
  }

  /* Check for /system/build.prop (exists on Android) */
  if (g_file_test("/system/build.prop", G_FILE_TEST_EXISTS)) {
    g_debug("nip55: Android detected via /system/build.prop");
    return TRUE;
  }

  /* Check for Termux environment */
  if (gnostr_android_is_termux()) {
    return TRUE;
  }

  g_debug("nip55: Android not detected");
  return FALSE;
}

gboolean gnostr_android_is_termux(void) {
  /* Termux sets PREFIX to its installation directory */
  const char *prefix = g_getenv("PREFIX");
  if (prefix && g_str_has_prefix(prefix, "/data/data/com.termux")) {
    g_debug("nip55: Termux detected via PREFIX=%s", prefix);
    return TRUE;
  }

  /* Alternative: check TERMUX_VERSION */
  const char *termux_version = g_getenv("TERMUX_VERSION");
  if (termux_version && *termux_version) {
    g_debug("nip55: Termux detected via TERMUX_VERSION=%s", termux_version);
    return TRUE;
  }

  return FALSE;
}

/**
 * URL-encode a string using GLib's g_uri_escape_string.
 * This encodes all special characters except unreserved ones.
 */
static char *url_encode(const char *str) {
  if (!str || !*str) {
    return g_strdup("");
  }

  /* Use GLib's URI escaping - encode everything except unreserved chars */
  /* The reserved_chars_allowed parameter is NULL to encode everything */
  return g_uri_escape_string(str, NULL, TRUE);
}

/**
 * Build the intent suffix with optional callback URI.
 */
static char *build_intent_suffix(const char *signer_package, const char *callback_uri) {
  const char *pkg = signer_package ? signer_package : DEFAULT_SIGNER_PACKAGE;

  if (callback_uri && *callback_uri) {
    char *encoded_callback = url_encode(callback_uri);
    char *suffix = g_strdup_printf(INTENT_SUFFIX_WITH_CALLBACK, pkg, encoded_callback);
    g_free(encoded_callback);
    return suffix;
  } else {
    return g_strdup_printf(INTENT_SUFFIX, pkg);
  }
}

char *gnostr_android_build_sign_intent(const char *unsigned_event_json,
                                        const char *signer_package,
                                        const char *callback_uri) {
  if (!unsigned_event_json || !*unsigned_event_json) {
    g_warning("nip55: cannot build sign intent with empty event JSON");
    return NULL;
  }

  /* Validate JSON before encoding */
  if (!nostr_json_is_valid(unsigned_event_json)) {
    g_warning("nip55: invalid event JSON for sign intent");
    return NULL;
  }

  /* URL-encode the event JSON */
  char *encoded_event = url_encode(unsigned_event_json);

  /* Build the intent URI */
  char *suffix = build_intent_suffix(signer_package, callback_uri);
  char *intent = g_strdup_printf("%s%s?%s=%s%s",
                                  INTENT_PREFIX, ACTION_SIGN,
                                  PARAM_EVENT, encoded_event,
                                  suffix);

  g_free(encoded_event);
  g_free(suffix);

  g_debug("nip55: built sign intent: %s", intent);
  return intent;
}

char *gnostr_android_build_encrypt_intent(const char *plaintext,
                                           const char *recipient_pubkey,
                                           GnostrAndroidEncryptionType encryption_type,
                                           const char *signer_package,
                                           const char *callback_uri) {
  if (!plaintext) {
    g_warning("nip55: cannot build encrypt intent with null plaintext");
    return NULL;
  }

  if (!recipient_pubkey || strlen(recipient_pubkey) != 64) {
    g_warning("nip55: cannot build encrypt intent with invalid pubkey");
    return NULL;
  }

  /* URL-encode parameters */
  char *encoded_plaintext = url_encode(plaintext);
  const char *type_str = gnostr_android_encryption_type_to_string(encryption_type);

  /* Build the intent URI */
  char *suffix = build_intent_suffix(signer_package, callback_uri);
  char *intent = g_strdup_printf("%s%s?%s=%s&%s=%s&%s=%s%s",
                                  INTENT_PREFIX, ACTION_ENCRYPT,
                                  PARAM_PLAINTEXT, encoded_plaintext,
                                  PARAM_PUBKEY, recipient_pubkey,
                                  PARAM_TYPE, type_str,
                                  suffix);

  g_free(encoded_plaintext);
  g_free(suffix);

  g_debug("nip55: built encrypt intent for pubkey %s", recipient_pubkey);
  return intent;
}

char *gnostr_android_build_decrypt_intent(const char *ciphertext,
                                           const char *sender_pubkey,
                                           GnostrAndroidEncryptionType encryption_type,
                                           const char *signer_package,
                                           const char *callback_uri) {
  if (!ciphertext || !*ciphertext) {
    g_warning("nip55: cannot build decrypt intent with empty ciphertext");
    return NULL;
  }

  if (!sender_pubkey || strlen(sender_pubkey) != 64) {
    g_warning("nip55: cannot build decrypt intent with invalid pubkey");
    return NULL;
  }

  /* URL-encode parameters */
  char *encoded_ciphertext = url_encode(ciphertext);
  const char *type_str = gnostr_android_encryption_type_to_string(encryption_type);

  /* Build the intent URI */
  char *suffix = build_intent_suffix(signer_package, callback_uri);
  char *intent = g_strdup_printf("%s%s?%s=%s&%s=%s&%s=%s%s",
                                  INTENT_PREFIX, ACTION_DECRYPT,
                                  PARAM_CIPHERTEXT, encoded_ciphertext,
                                  PARAM_PUBKEY, sender_pubkey,
                                  PARAM_TYPE, type_str,
                                  suffix);

  g_free(encoded_ciphertext);
  g_free(suffix);

  g_debug("nip55: built decrypt intent for pubkey %s", sender_pubkey);
  return intent;
}

char *gnostr_android_build_get_public_key_intent(const char *signer_package,
                                                  const char *callback_uri) {
  /* Build the intent URI - no parameters needed */
  char *suffix = build_intent_suffix(signer_package, callback_uri);
  char *intent = g_strdup_printf("%s%s%s",
                                  INTENT_PREFIX, ACTION_GET_PUBLIC_KEY,
                                  suffix);
  g_free(suffix);

  g_debug("nip55: built get_public_key intent");
  return intent;
}

/**
 * Parse query parameters from a URI.
 * Returns a hash table of key=value pairs.
 */
static GHashTable *parse_query_params(const char *uri) {
  GHashTable *params = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

  /* Find the query string (after '?') */
  const char *query_start = strchr(uri, '?');
  if (!query_start) {
    return params;
  }
  query_start++; /* Skip the '?' */

  /* Find the end of the query string (before '#' if present) */
  const char *query_end = strchr(query_start, '#');
  size_t query_len = query_end ? (size_t)(query_end - query_start) : strlen(query_start);

  /* Make a copy of the query string to tokenize */
  char *query = g_strndup(query_start, query_len);

  /* Split on '&' */
  char **pairs = g_strsplit(query, "&", -1);
  g_free(query);

  for (int i = 0; pairs[i]; i++) {
    char *eq = strchr(pairs[i], '=');
    if (eq) {
      char *key = g_strndup(pairs[i], eq - pairs[i]);
      char *value = g_uri_unescape_string(eq + 1, NULL);
      if (value) {
        g_hash_table_insert(params, key, value);
      } else {
        g_free(key);
      }
    }
  }

  g_strfreev(pairs);
  return params;
}

GnostrAndroidSignerResponse *gnostr_android_parse_response(const char *response_uri,
                                                            GnostrAndroidSignerRequest request_type) {
  if (!response_uri || !*response_uri) {
    return NULL;
  }

  GnostrAndroidSignerResponse *response = g_new0(GnostrAndroidSignerResponse, 1);
  response->success = FALSE;

  /* Parse query parameters */
  GHashTable *params = parse_query_params(response_uri);

  /* Check for error */
  const char *error = g_hash_table_lookup(params, "error");
  if (error && *error) {
    response->error_message = g_strdup(error);
    g_hash_table_unref(params);
    return response;
  }

  /* Parse based on request type */
  switch (request_type) {
    case GNOSTR_ANDROID_SIGNER_SIGN: {
      /* Look for signature or signed event */
      const char *sig = g_hash_table_lookup(params, "sig");
      const char *event = g_hash_table_lookup(params, "event");
      const char *result = g_hash_table_lookup(params, "result");

      if (sig && strlen(sig) == 128) {
        response->signature = g_strdup(sig);
        response->success = TRUE;
      } else if (event && *event) {
        /* Parse the event JSON to extract signature */
        GnostrAndroidSignerResponse *json_response = gnostr_android_parse_sign_response_json(event);
        if (json_response) {
          g_free(response);
          response = json_response;
        }
      } else if (result && *result) {
        /* Some signers return the full result in 'result' parameter */
        GnostrAndroidSignerResponse *json_response = gnostr_android_parse_sign_response_json(result);
        if (json_response) {
          g_free(response);
          response = json_response;
        }
      }
      break;
    }

    case GNOSTR_ANDROID_SIGNER_ENCRYPT:
    case GNOSTR_ANDROID_SIGNER_DECRYPT: {
      /* Look for result text */
      const char *result = g_hash_table_lookup(params, "result");
      if (result && *result) {
        response->result_text = g_strdup(result);
        response->success = TRUE;
      }
      break;
    }

    case GNOSTR_ANDROID_SIGNER_GET_PUBLIC_KEY: {
      /* Look for public key */
      const char *pubkey = g_hash_table_lookup(params, "pubKey");
      const char *npub = g_hash_table_lookup(params, "npub");

      if (pubkey && strlen(pubkey) == 64) {
        response->pubkey_hex = g_strdup(pubkey);
        response->success = TRUE;
      }
      if (npub && g_str_has_prefix(npub, "npub1")) {
        response->npub = g_strdup(npub);
        if (!response->pubkey_hex) {
          /* If we only have npub, mark as success but note we need to decode it */
          response->success = TRUE;
        }
      }
      break;
    }
  }

  g_hash_table_unref(params);
  return response;
}

GnostrAndroidSignerResponse *gnostr_android_parse_sign_response_json(const char *json_str) {
  if (!json_str || !*json_str) {
    return NULL;
  }

  GnostrAndroidSignerResponse *response = g_new0(GnostrAndroidSignerResponse, 1);
  response->success = FALSE;

  /* Validate JSON first */
  if (!nostr_json_is_valid(json_str)) {
    g_warning("nip55: failed to parse sign response JSON");
    response->error_message = g_strdup("JSON parse error");
    return response;
  }

  /* Check if this is a full event or just a signature object */
  char *sig = NULL;
  if (nostr_json_get_string(json_str, "sig", &sig) == 0 && sig) {
    if (strlen(sig) == 128) {
      response->signature = sig;
      sig = NULL; /* ownership transferred */
      response->success = TRUE;

      /* Check if this is a full event (has 'id', 'pubkey', 'kind', etc.) */
      char *id_str = NULL;
      char *pubkey_str = NULL;
      int kind = 0;
      gboolean has_id = (nostr_json_get_string(json_str, "id", &id_str) == 0 && id_str);
      gboolean has_pubkey = (nostr_json_get_string(json_str, "pubkey", &pubkey_str) == 0 && pubkey_str);
      gboolean has_kind = (nostr_json_get_int(json_str, "kind", &kind) == 0);

      if (has_id && has_pubkey && has_kind) {
        /* This is a full signed event, store the JSON */
        response->signed_event_json = g_strdup(json_str);
        g_debug("nip55: parsed full signed event with id=%s", id_str);
      } else {
        g_debug("nip55: parsed signature-only response");
      }

      g_free(id_str);
      g_free(pubkey_str);
    } else {
      g_free(sig);
    }
  }

  /* Check for error field */
  char *err_str = NULL;
  if (nostr_json_get_string(json_str, "error", &err_str) == 0 && err_str && *err_str) {
    response->error_message = err_str;
    err_str = NULL; /* ownership transferred */
    response->success = FALSE;
  }
  g_free(err_str);

  return response;
}

void gnostr_android_signer_response_free(GnostrAndroidSignerResponse *response) {
  if (!response) {
    return;
  }

  g_free(response->error_message);
  g_free(response->signature);
  g_free(response->signed_event_json);
  g_free(response->result_text);
  g_free(response->pubkey_hex);
  g_free(response->npub);
  g_free(response);
}
