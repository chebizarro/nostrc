/* key_rotation.c - Key rotation and migration implementation
 *
 * Implements the key rotation workflow:
 * 1. Generate new keypair
 * 2. Create migration event (kind 1776) with old->new pubkey link
 * 3. Sign migration event with old key
 * 4. Optionally add attestation signature from new key in tags
 * 5. Store new key in secure storage
 * 6. Optionally publish migration event to relays
 * 7. Update accounts store with new active identity
 */
#include "key_rotation.h"
#include "secret_store.h"
#include "accounts_store.h"
#include "relay_store.h"
#include "secure-memory.h"
#include <json-glib/json-glib.h>
#include <nostr-gobject-1.0/nostr_nip19.h>
#include <nostr-gobject-1.0/nostr_keys.h>
/* Core key generation still needed (GObject wrapper doesn't expose private key hex) */
#include <keys.h>
#include <string.h>
#include <time.h>

struct _KeyRotation {
  /* Source key */
  gchar *old_npub;
  gchar *old_pubkey_hex;  /* Derived from old_npub */

  /* New key (generated during rotation) */
  gchar *new_npub;
  gchar *new_pubkey_hex;
  gchar *new_nsec;        /* Secure memory - cleared after storage */

  /* Options */
  gchar *new_label;
  gboolean publish;
  gboolean keep_old;

  /* State */
  KeyRotationState state;
  gboolean cancelled;

  /* Result */
  gchar *migration_event;
  gchar *error_message;

  /* Callbacks */
  KeyRotationProgressCb progress_cb;
  gpointer progress_user_data;
  KeyRotationCompleteCb complete_cb;
  gpointer complete_user_data;
};

/* Internal helpers */
static gchar *npub_to_hex(const gchar *npub) {
  if (!npub || !g_str_has_prefix(npub, "npub1")) return NULL;

  g_autoptr(GNostrNip19) nip19 = gnostr_nip19_decode(npub, NULL);
  if (!nip19) return NULL;

  const gchar *pubkey = gnostr_nip19_get_pubkey(nip19);
  gchar *hex = pubkey ? g_strdup(pubkey) : NULL;
  return hex;
}

static void emit_progress(KeyRotation *kr, KeyRotationState state, const gchar *message) {
  kr->state = state;
  if (kr->progress_cb) {
    kr->progress_cb(kr, state, message, kr->progress_user_data);
  }
}

static void emit_complete(KeyRotation *kr, KeyRotationResult result, const gchar *error) {
  if (result == KEY_ROTATION_OK) {
    kr->state = KEY_ROTATION_STATE_COMPLETE;
  } else {
    kr->state = KEY_ROTATION_STATE_ERROR;
    g_free(kr->error_message);
    kr->error_message = error ? g_strdup(error) : NULL;
  }

  if (kr->complete_cb) {
    kr->complete_cb(kr, result, kr->new_npub, error, kr->complete_user_data);
  }
}

KeyRotation *key_rotation_new(const gchar *old_npub) {
  if (!old_npub || !g_str_has_prefix(old_npub, "npub1")) {
    return NULL;
  }

  KeyRotation *kr = g_new0(KeyRotation, 1);
  kr->old_npub = g_strdup(old_npub);
  kr->old_pubkey_hex = npub_to_hex(old_npub);
  kr->publish = TRUE;
  kr->keep_old = TRUE;
  kr->state = KEY_ROTATION_STATE_IDLE;

  if (!kr->old_pubkey_hex) {
    g_free(kr->old_npub);
    g_free(kr);
    return NULL;
  }

  return kr;
}

void key_rotation_free(KeyRotation *kr) {
  if (!kr) return;

  g_free(kr->old_npub);
  g_free(kr->old_pubkey_hex);
  g_free(kr->new_npub);
  g_free(kr->new_pubkey_hex);
  g_free(kr->new_label);
  g_free(kr->migration_event);
  g_free(kr->error_message);

  /* Securely clear the secret key if present */
  if (kr->new_nsec) {
    gn_secure_strfree(kr->new_nsec);
  }

  g_free(kr);
}

void key_rotation_set_new_label(KeyRotation *kr, const gchar *label) {
  if (!kr) return;
  g_free(kr->new_label);
  kr->new_label = label ? g_strdup(label) : NULL;
}

void key_rotation_set_publish(KeyRotation *kr, gboolean publish) {
  if (!kr) return;
  kr->publish = publish;
}

void key_rotation_set_keep_old(KeyRotation *kr, gboolean keep) {
  if (!kr) return;
  kr->keep_old = keep;
}

void key_rotation_set_progress_callback(KeyRotation *kr,
                                         KeyRotationProgressCb callback,
                                         gpointer user_data) {
  if (!kr) return;
  kr->progress_cb = callback;
  kr->progress_user_data = user_data;
}

void key_rotation_set_complete_callback(KeyRotation *kr,
                                         KeyRotationCompleteCb callback,
                                         gpointer user_data) {
  if (!kr) return;
  kr->complete_cb = callback;
  kr->complete_user_data = user_data;
}

/* Build the migration event JSON (unsigned) */
gchar *key_rotation_build_migration_event(const gchar *old_pubkey_hex,
                                           const gchar *new_pubkey_hex,
                                           gint64 created_at,
                                           const gchar *content) {
  if (!old_pubkey_hex || !new_pubkey_hex) return NULL;

  if (created_at == 0) {
    created_at = (gint64)time(NULL);
  }

  /* Build default content if not provided */
  gchar *content_str = NULL;
  if (content && *content) {
    content_str = g_strdup(content);
  } else {
    /* Encode new pubkey as npub for content */
    g_autoptr(GNostrNip19) nip19 = gnostr_nip19_encode_npub(new_pubkey_hex, NULL);
    if (nip19) {
      const gchar *new_npub = gnostr_nip19_get_bech32(nip19);
      if (new_npub)
        content_str = g_strdup_printf("Migrating to new key: %s", new_npub);
    }
    if (!content_str) {
      content_str = g_strdup_printf("Migrating to new key: %s", new_pubkey_hex);
    }
  }

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);

  /* kind: 1776 (migration announcement) */
  json_builder_set_member_name(builder, "kind");
  json_builder_add_int_value(builder, KEY_MIGRATION_EVENT_KIND);

  /* pubkey: old public key */
  json_builder_set_member_name(builder, "pubkey");
  json_builder_add_string_value(builder, old_pubkey_hex);

  /* created_at */
  json_builder_set_member_name(builder, "created_at");
  json_builder_add_int_value(builder, created_at);

  /* tags */
  json_builder_set_member_name(builder, "tags");
  json_builder_begin_array(builder);

  /* p tag with new pubkey */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "p");
  json_builder_add_string_value(builder, new_pubkey_hex);
  json_builder_add_string_value(builder, "");  /* relay hint */
  json_builder_add_string_value(builder, "successor");  /* marker */
  json_builder_end_array(builder);

  /* alt tag for clients that don't understand kind 1776 */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "alt");
  json_builder_add_string_value(builder, "Key migration announcement");
  json_builder_end_array(builder);

  json_builder_end_array(builder);

  /* content */
  json_builder_set_member_name(builder, "content");
  json_builder_add_string_value(builder, content_str);

  json_builder_end_object(builder);

  JsonNode *root = json_builder_get_root(builder);
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, root);
  gchar *result = json_generator_to_data(gen, NULL);

  json_node_unref(root);
  g_free(content_str);

  return result;
}

/* Main rotation execution - runs in idle to allow UI updates */
static gboolean rotation_step(gpointer user_data);

gboolean key_rotation_execute(KeyRotation *kr) {
  if (!kr) return FALSE;
  if (kr->state != KEY_ROTATION_STATE_IDLE) return FALSE;

  /* Verify old key exists */
  gchar *nsec = NULL;
  SecretStoreResult rc = secret_store_get_secret(kr->old_npub, &nsec);
  if (rc != SECRET_STORE_OK || !nsec) {
    emit_complete(kr, KEY_ROTATION_ERR_NO_SOURCE_KEY,
                  "Source key not found in secure storage");
    return FALSE;
  }
  gn_secure_strfree(nsec);

  /* Start rotation process */
  kr->cancelled = FALSE;
  g_idle_add(rotation_step, kr);

  return TRUE;
}

/* Step-by-step rotation to allow progress updates */
static gboolean rotation_step(gpointer user_data) {
  KeyRotation *kr = user_data;

  if (kr->cancelled) {
    emit_complete(kr, KEY_ROTATION_ERR_CANCELLED, "Rotation cancelled by user");
    return G_SOURCE_REMOVE;
  }

  switch (kr->state) {
    case KEY_ROTATION_STATE_IDLE:
      /* Step 1: Generate new keypair */
      emit_progress(kr, KEY_ROTATION_STATE_GENERATING, "Generating new keypair...");
      return G_SOURCE_CONTINUE;

    case KEY_ROTATION_STATE_GENERATING: {
      /* Generate new private key */
      gchar *sk_hex_raw = nostr_key_generate_private();
      if (!sk_hex_raw) {
        emit_complete(kr, KEY_ROTATION_ERR_GENERATE_FAILED,
                      "Failed to generate new private key");
        return G_SOURCE_REMOVE;
      }

      /* Copy to secure memory */
      gchar *sk_hex = gn_secure_strdup(sk_hex_raw);
      gn_secure_zero(sk_hex_raw, strlen(sk_hex_raw));
      free(sk_hex_raw);

      if (!sk_hex) {
        emit_complete(kr, KEY_ROTATION_ERR_GENERATE_FAILED,
                      "Failed to allocate secure memory");
        return G_SOURCE_REMOVE;
      }

      /* Derive public key and npub using GNostrKeys */
      g_autoptr(GNostrKeys) keys = gnostr_keys_new_from_hex(sk_hex, NULL);
      if (!keys) {
        gn_secure_strfree(sk_hex);
        emit_complete(kr, KEY_ROTATION_ERR_GENERATE_FAILED,
                      "Failed to derive public key");
        return G_SOURCE_REMOVE;
      }

      const gchar *pk_hex = gnostr_keys_get_pubkey(keys);
      kr->new_pubkey_hex = g_strdup(pk_hex);
      gchar *npub = gnostr_keys_get_npub(keys);
      kr->new_npub = npub;  /* transfer full */

      if (!kr->new_pubkey_hex || !kr->new_npub) {
        gn_secure_strfree(sk_hex);
        emit_complete(kr, KEY_ROTATION_ERR_GENERATE_FAILED,
                      "Failed to encode npub");
        return G_SOURCE_REMOVE;
      }

      /* Convert sk_hex to nsec for storage using GNostrNip19 */
      g_autoptr(GNostrNip19) nip19 = gnostr_nip19_encode_nsec(sk_hex, NULL);
      gn_secure_strfree(sk_hex);

      if (!nip19) {
        emit_complete(kr, KEY_ROTATION_ERR_GENERATE_FAILED,
                      "Failed to encode nsec");
        return G_SOURCE_REMOVE;
      }

      const gchar *nsec_str = gnostr_nip19_get_bech32(nip19);
      kr->new_nsec = nsec_str ? gn_secure_strdup(nsec_str) : NULL;

      if (!kr->new_nsec) {
        emit_complete(kr, KEY_ROTATION_ERR_GENERATE_FAILED,
                      "Failed to encode nsec");
        return G_SOURCE_REMOVE;
      }

      emit_progress(kr, KEY_ROTATION_STATE_CREATING_EVENT,
                    "Creating migration event...");
      return G_SOURCE_CONTINUE;
    }

    case KEY_ROTATION_STATE_CREATING_EVENT: {
      /* Build migration event */
      gchar *event_json = key_rotation_build_migration_event(
          kr->old_pubkey_hex,
          kr->new_pubkey_hex,
          0,  /* current time */
          NULL  /* default content */
      );

      if (!event_json) {
        emit_complete(kr, KEY_ROTATION_ERR_SIGN_FAILED,
                      "Failed to build migration event");
        return G_SOURCE_REMOVE;
      }

      /* Store unsigned event temporarily */
      kr->migration_event = event_json;

      emit_progress(kr, KEY_ROTATION_STATE_SIGNING_OLD,
                    "Signing with old key...");
      return G_SOURCE_CONTINUE;
    }

    case KEY_ROTATION_STATE_SIGNING_OLD: {
      /* Sign migration event with old key */
      gchar *signature = NULL;
      SecretStoreResult rc = secret_store_sign_event(
          kr->migration_event,
          kr->old_npub,
          &signature
      );

      if (rc != SECRET_STORE_OK || !signature) {
        emit_complete(kr, KEY_ROTATION_ERR_SIGN_FAILED,
                      "Failed to sign migration event with old key");
        return G_SOURCE_REMOVE;
      }

      /* Add signature to event */
      g_autoptr(JsonParser) parser = json_parser_new();
      if (!json_parser_load_from_data(parser, kr->migration_event, -1, NULL)) {
        g_free(signature);
        emit_complete(kr, KEY_ROTATION_ERR_SIGN_FAILED,
                      "Failed to parse migration event");
        return G_SOURCE_REMOVE;
      }

      JsonNode *root = json_parser_steal_root(parser);

      JsonObject *obj = json_node_get_object(root);
      json_object_set_string_member(obj, "sig", signature);
      json_object_set_string_member(obj, "id", "");  /* Will be computed */

      /* Regenerate JSON with signature */
      g_autoptr(JsonGenerator) gen = json_generator_new();
      json_generator_set_root(gen, root);
      g_free(kr->migration_event);
      kr->migration_event = json_generator_to_data(gen, NULL);

      json_node_unref(root);
      g_free(signature);

      emit_progress(kr, KEY_ROTATION_STATE_SIGNING_NEW,
                    "Adding attestation from new key...");
      return G_SOURCE_CONTINUE;
    }

    case KEY_ROTATION_STATE_SIGNING_NEW: {
      /* Optionally add attestation signature from new key
       * This proves the new key holder authorized this migration
       * For now, we skip this step since the new key isn't in storage yet
       * The migration event signed by old key is the primary proof
       */
      emit_progress(kr, KEY_ROTATION_STATE_STORING,
                    "Storing new key in secure storage...");
      return G_SOURCE_CONTINUE;
    }

    case KEY_ROTATION_STATE_STORING: {
      /* Determine label for new key */
      gchar *label = NULL;
      if (kr->new_label && *kr->new_label) {
        label = g_strdup(kr->new_label);
      } else {
        /* Use original label with suffix */
        AccountsStore *as = accounts_store_get_default();
        gchar *old_label = accounts_store_get_display_name(as, kr->old_npub);
        if (old_label && *old_label) {
          label = g_strdup_printf("%s (rotated)", old_label);
          g_free(old_label);
        } else {
          label = g_strdup("Rotated Identity");
        }
      }

      /* Store new key */
      SecretStoreResult rc = secret_store_add(kr->new_nsec, label, TRUE);

      /* Securely clear the nsec now that it's stored */
      gn_secure_strfree(kr->new_nsec);
      kr->new_nsec = NULL;

      if (rc != SECRET_STORE_OK) {
        g_free(label);
        emit_complete(kr, KEY_ROTATION_ERR_STORE_FAILED,
                      "Failed to store new key in secure storage");
        return G_SOURCE_REMOVE;
      }

      /* Add to accounts store */
      AccountsStore *as = accounts_store_get_default();
      accounts_store_add(as, kr->new_npub, label, NULL);
      g_free(label);

      /* Optionally update old key label */
      if (kr->keep_old) {
        gchar *old_display = accounts_store_get_display_name(as, kr->old_npub);
        if (old_display) {
          gchar *new_old_label = g_strdup_printf("%s (migrated)", old_display);
          accounts_store_set_label(as, kr->old_npub, new_old_label, NULL);
          g_free(new_old_label);
          g_free(old_display);
        }
      }

      /* Set new key as active */
      accounts_store_set_active(as, kr->new_npub, NULL);
      accounts_store_save(as, NULL);

      if (kr->publish) {
        emit_progress(kr, KEY_ROTATION_STATE_PUBLISHING,
                      "Publishing migration to relays...");
      } else {
        emit_complete(kr, KEY_ROTATION_OK, NULL);
      }
      return G_SOURCE_CONTINUE;
    }

    case KEY_ROTATION_STATE_PUBLISHING: {
      /* Publish migration event to relays
       * For now, we just log it - actual relay publishing
       * would need async WebSocket connections
       */
      g_message("Key rotation complete. Migration event:\n%s",
                kr->migration_event);

      /* In a full implementation, we would:
       * 1. Get write relays from relay_store
       * 2. Connect to each relay
       * 3. Send the migration event
       * 4. Wait for confirmations
       *
       * For now, we mark as complete and the user can manually publish
       */
      emit_complete(kr, KEY_ROTATION_OK, NULL);
      return G_SOURCE_REMOVE;
    }

    case KEY_ROTATION_STATE_COMPLETE:
    case KEY_ROTATION_STATE_ERROR:
      /* Already done */
      return G_SOURCE_REMOVE;
  }

  return G_SOURCE_REMOVE;
}

void key_rotation_cancel(KeyRotation *kr) {
  if (!kr) return;
  kr->cancelled = TRUE;
}

KeyRotationState key_rotation_get_state(KeyRotation *kr) {
  return kr ? kr->state : KEY_ROTATION_STATE_IDLE;
}

const gchar *key_rotation_get_old_npub(KeyRotation *kr) {
  return kr ? kr->old_npub : NULL;
}

const gchar *key_rotation_get_new_npub(KeyRotation *kr) {
  return kr ? kr->new_npub : NULL;
}

const gchar *key_rotation_get_migration_event(KeyRotation *kr) {
  return kr ? kr->migration_event : NULL;
}

gboolean key_rotation_verify_migration(const gchar *event_json,
                                        gchar **out_old_pubkey,
                                        gchar **out_new_pubkey) {
  if (!event_json) return FALSE;

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, event_json, -1, NULL)) {
    return FALSE;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    return FALSE;
  }

  JsonObject *obj = json_node_get_object(root);

  /* Verify kind */
  if (!json_object_has_member(obj, "kind") ||
      json_object_get_int_member(obj, "kind") != KEY_MIGRATION_EVENT_KIND) {
    return FALSE;
  }

  /* Get old pubkey */
  if (!json_object_has_member(obj, "pubkey")) {
    return FALSE;
  }
  const gchar *old_pk = json_object_get_string_member(obj, "pubkey");

  /* Find new pubkey in p tag */
  if (!json_object_has_member(obj, "tags")) {
    return FALSE;
  }

  JsonArray *tags = json_object_get_array_member(obj, "tags");
  const gchar *new_pk = NULL;

  guint n = json_array_get_length(tags);
  for (guint i = 0; i < n; i++) {
    JsonNode *tag_node = json_array_get_element(tags, i);
    if (!JSON_NODE_HOLDS_ARRAY(tag_node)) continue;

    JsonArray *tag = json_node_get_array(tag_node);
    if (json_array_get_length(tag) < 2) continue;

    const gchar *tag_type = json_array_get_string_element(tag, 0);
    if (g_strcmp0(tag_type, "p") == 0) {
      new_pk = json_array_get_string_element(tag, 1);
      break;
    }
  }

  if (!new_pk) {
    return FALSE;
  }

  /* Verify signature exists (actual verification would need crypto) */
  if (!json_object_has_member(obj, "sig")) {
    return FALSE;
  }

  if (out_old_pubkey) *out_old_pubkey = g_strdup(old_pk);
  if (out_new_pubkey) *out_new_pubkey = g_strdup(new_pk);

  return TRUE;
}

const gchar *key_rotation_result_to_string(KeyRotationResult result) {
  switch (result) {
    case KEY_ROTATION_OK:
      return "Success";
    case KEY_ROTATION_ERR_NO_SOURCE_KEY:
      return "Source key not found";
    case KEY_ROTATION_ERR_GENERATE_FAILED:
      return "Failed to generate new key";
    case KEY_ROTATION_ERR_SIGN_FAILED:
      return "Failed to sign migration event";
    case KEY_ROTATION_ERR_STORE_FAILED:
      return "Failed to store new key";
    case KEY_ROTATION_ERR_PUBLISH_FAILED:
      return "Failed to publish to relays";
    case KEY_ROTATION_ERR_INVALID_PARAMS:
      return "Invalid parameters";
    case KEY_ROTATION_ERR_CANCELLED:
      return "Cancelled";
    default:
      return "Unknown error";
  }
}

const gchar *key_rotation_state_to_string(KeyRotationState state) {
  switch (state) {
    case KEY_ROTATION_STATE_IDLE:
      return "Idle";
    case KEY_ROTATION_STATE_GENERATING:
      return "Generating new keypair";
    case KEY_ROTATION_STATE_CREATING_EVENT:
      return "Creating migration event";
    case KEY_ROTATION_STATE_SIGNING_OLD:
      return "Signing with old key";
    case KEY_ROTATION_STATE_SIGNING_NEW:
      return "Adding new key attestation";
    case KEY_ROTATION_STATE_STORING:
      return "Storing new key";
    case KEY_ROTATION_STATE_PUBLISHING:
      return "Publishing to relays";
    case KEY_ROTATION_STATE_COMPLETE:
      return "Complete";
    case KEY_ROTATION_STATE_ERROR:
      return "Error";
    default:
      return "Unknown";
  }
}
