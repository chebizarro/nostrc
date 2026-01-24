/**
 * NIP-61: Nutzaps (Ecash Zaps) Implementation
 *
 * Implements nutzap preferences parsing, nutzap event parsing,
 * and event building for Cashu ecash zaps on Nostr.
 */

#define G_LOG_DOMAIN "nip61-nutzaps"

#include "nip61_nutzaps.h"
#include <jansson.h>
#include <string.h>
#include <time.h>

/* ============== Nutzap Mint ============== */

GnostrNutzapMint *
gnostr_nutzap_mint_new(void)
{
  return g_new0(GnostrNutzapMint, 1);
}

GnostrNutzapMint *
gnostr_nutzap_mint_new_full(const gchar *url,
                             const gchar *unit,
                             const gchar *pubkey)
{
  GnostrNutzapMint *mint = gnostr_nutzap_mint_new();
  mint->url = g_strdup(url);
  mint->unit = g_strdup(unit);
  mint->pubkey = pubkey ? g_strdup(pubkey) : NULL;
  return mint;
}

void
gnostr_nutzap_mint_free(GnostrNutzapMint *mint)
{
  if (!mint) return;
  g_free(mint->url);
  g_free(mint->unit);
  g_free(mint->pubkey);
  g_free(mint);
}

/* ============== Nutzap Preferences ============== */

GnostrNutzapPrefs *
gnostr_nutzap_prefs_new(void)
{
  return g_new0(GnostrNutzapPrefs, 1);
}

void
gnostr_nutzap_prefs_free(GnostrNutzapPrefs *prefs)
{
  if (!prefs) return;

  /* Free mints array */
  if (prefs->mints) {
    for (gsize i = 0; i < prefs->mint_count; i++) {
      gnostr_nutzap_mint_free(prefs->mints[i]);
    }
    g_free(prefs->mints);
  }

  /* Free relays array */
  if (prefs->relays) {
    for (gsize i = 0; i < prefs->relay_count; i++) {
      g_free(prefs->relays[i]);
    }
    g_free(prefs->relays);
  }

  g_free(prefs);
}

GnostrNutzapPrefs *
gnostr_nutzap_prefs_parse(const gchar *event_json)
{
  if (!event_json || !*event_json) return NULL;

  json_error_t error;
  json_t *root = json_loads(event_json, 0, &error);
  if (!root) {
    g_warning("nutzap_prefs: failed to parse JSON: %s", error.text);
    return NULL;
  }

  /* Verify kind 10019 */
  json_t *kind_val = json_object_get(root, "kind");
  if (!kind_val || json_integer_value(kind_val) != NIP61_KIND_NUTZAP_PREFS) {
    g_debug("nutzap_prefs: wrong kind, expected %d", NIP61_KIND_NUTZAP_PREFS);
    json_decref(root);
    return NULL;
  }

  GnostrNutzapPrefs *prefs = gnostr_nutzap_prefs_new();

  /* Temporary arrays for collecting mints and relays */
  GPtrArray *mints_arr = g_ptr_array_new();
  GPtrArray *relays_arr = g_ptr_array_new_with_free_func(g_free);

  /* Parse tags */
  json_t *tags = json_object_get(root, "tags");
  if (tags && json_is_array(tags)) {
    size_t i;
    json_t *tag;
    json_array_foreach(tags, i, tag) {
      if (!json_is_array(tag) || json_array_size(tag) < 1) continue;

      const char *tag_name = json_string_value(json_array_get(tag, 0));
      if (!tag_name) continue;

      if (g_strcmp0(tag_name, "mint") == 0) {
        /* ["mint", "<url>", "<unit>", "<optional-pubkey>"] */
        if (json_array_size(tag) >= 3) {
          const char *url = json_string_value(json_array_get(tag, 1));
          const char *unit = json_string_value(json_array_get(tag, 2));
          const char *pubkey = NULL;

          if (json_array_size(tag) >= 4) {
            pubkey = json_string_value(json_array_get(tag, 3));
          }

          if (url && unit) {
            GnostrNutzapMint *mint = gnostr_nutzap_mint_new_full(url, unit, pubkey);
            g_ptr_array_add(mints_arr, mint);
            g_debug("nutzap_prefs: parsed mint url=%s unit=%s", url, unit);
          }
        }
      } else if (g_strcmp0(tag_name, "relay") == 0) {
        /* ["relay", "<url>"] */
        if (json_array_size(tag) >= 2) {
          const char *relay_url = json_string_value(json_array_get(tag, 1));
          if (relay_url && *relay_url) {
            g_ptr_array_add(relays_arr, g_strdup(relay_url));
            g_debug("nutzap_prefs: parsed relay=%s", relay_url);
          }
        }
      } else if (g_strcmp0(tag_name, "p2pk") == 0) {
        /* ["p2pk"] - presence indicates requirement */
        prefs->require_p2pk = TRUE;
        g_debug("nutzap_prefs: p2pk required");
      }
    }
  }

  /* Transfer mints to prefs */
  prefs->mint_count = mints_arr->len;
  if (prefs->mint_count > 0) {
    prefs->mints = g_new(GnostrNutzapMint *, prefs->mint_count);
    for (gsize i = 0; i < prefs->mint_count; i++) {
      prefs->mints[i] = g_ptr_array_index(mints_arr, i);
    }
  }
  g_ptr_array_free(mints_arr, TRUE);

  /* Transfer relays to prefs */
  prefs->relay_count = relays_arr->len;
  if (prefs->relay_count > 0) {
    prefs->relays = g_new(gchar *, prefs->relay_count);
    for (gsize i = 0; i < prefs->relay_count; i++) {
      prefs->relays[i] = g_ptr_array_index(relays_arr, i);
    }
  }
  /* Don't free the strings, just the array */
  g_ptr_array_free(relays_arr, TRUE);

  json_decref(root);

  g_debug("nutzap_prefs: parsed %zu mints, %zu relays, p2pk=%d",
          prefs->mint_count, prefs->relay_count, prefs->require_p2pk);

  return prefs;
}

void
gnostr_nutzap_prefs_add_mint(GnostrNutzapPrefs *prefs,
                              GnostrNutzapMint *mint)
{
  g_return_if_fail(prefs != NULL);
  g_return_if_fail(mint != NULL);

  prefs->mints = g_renew(GnostrNutzapMint *, prefs->mints, prefs->mint_count + 1);
  prefs->mints[prefs->mint_count] = mint;
  prefs->mint_count++;
}

void
gnostr_nutzap_prefs_add_relay(GnostrNutzapPrefs *prefs,
                               const gchar *relay_url)
{
  g_return_if_fail(prefs != NULL);
  g_return_if_fail(relay_url != NULL && *relay_url);

  prefs->relays = g_renew(gchar *, prefs->relays, prefs->relay_count + 1);
  prefs->relays[prefs->relay_count] = g_strdup(relay_url);
  prefs->relay_count++;
}

GPtrArray *
gnostr_nutzap_prefs_build_tags(const GnostrNutzapPrefs *prefs)
{
  g_return_val_if_fail(prefs != NULL, NULL);

  GPtrArray *tags = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);

  /* Add mint tags */
  for (gsize i = 0; i < prefs->mint_count; i++) {
    GnostrNutzapMint *mint = prefs->mints[i];
    GPtrArray *tag = g_ptr_array_new_with_free_func(g_free);

    g_ptr_array_add(tag, g_strdup("mint"));
    g_ptr_array_add(tag, g_strdup(mint->url ? mint->url : ""));
    g_ptr_array_add(tag, g_strdup(mint->unit ? mint->unit : "sat"));

    if (mint->pubkey && *mint->pubkey) {
      g_ptr_array_add(tag, g_strdup(mint->pubkey));
    }

    g_ptr_array_add(tags, tag);
  }

  /* Add relay tags */
  for (gsize i = 0; i < prefs->relay_count; i++) {
    GPtrArray *tag = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(tag, g_strdup("relay"));
    g_ptr_array_add(tag, g_strdup(prefs->relays[i]));
    g_ptr_array_add(tags, tag);
  }

  /* Add p2pk tag if required */
  if (prefs->require_p2pk) {
    GPtrArray *tag = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(tag, g_strdup("p2pk"));
    g_ptr_array_add(tags, tag);
  }

  return tags;
}

gchar *
gnostr_nutzap_prefs_build_event_json(const GnostrNutzapPrefs *prefs,
                                      const gchar *pubkey)
{
  g_return_val_if_fail(prefs != NULL, NULL);
  g_return_val_if_fail(pubkey != NULL && strlen(pubkey) == 64, NULL);

  json_t *event = json_object();

  /* Kind 10019 - nutzap preferences */
  json_object_set_new(event, "kind", json_integer(NIP61_KIND_NUTZAP_PREFS));

  /* Content - empty per spec */
  json_object_set_new(event, "content", json_string(""));

  /* Pubkey */
  json_object_set_new(event, "pubkey", json_string(pubkey));

  /* Created at */
  json_object_set_new(event, "created_at", json_integer((json_int_t)time(NULL)));

  /* Tags */
  json_t *tags = json_array();

  /* Add mint tags */
  for (gsize i = 0; i < prefs->mint_count; i++) {
    GnostrNutzapMint *mint = prefs->mints[i];
    json_t *tag = json_array();

    json_array_append_new(tag, json_string("mint"));
    json_array_append_new(tag, json_string(mint->url ? mint->url : ""));
    json_array_append_new(tag, json_string(mint->unit ? mint->unit : "sat"));

    if (mint->pubkey && *mint->pubkey) {
      json_array_append_new(tag, json_string(mint->pubkey));
    }

    json_array_append_new(tags, tag);
  }

  /* Add relay tags */
  for (gsize i = 0; i < prefs->relay_count; i++) {
    json_t *tag = json_array();
    json_array_append_new(tag, json_string("relay"));
    json_array_append_new(tag, json_string(prefs->relays[i]));
    json_array_append_new(tags, tag);
  }

  /* Add p2pk tag if required */
  if (prefs->require_p2pk) {
    json_t *tag = json_array();
    json_array_append_new(tag, json_string("p2pk"));
    json_array_append_new(tags, tag);
  }

  json_object_set_new(event, "tags", tags);

  gchar *result = json_dumps(event, JSON_COMPACT);
  json_decref(event);

  return result;
}

gboolean
gnostr_nutzap_prefs_accepts_mint(const GnostrNutzapPrefs *prefs,
                                  const gchar *mint_url)
{
  g_return_val_if_fail(prefs != NULL, FALSE);
  g_return_val_if_fail(mint_url != NULL, FALSE);

  for (gsize i = 0; i < prefs->mint_count; i++) {
    if (prefs->mints[i]->url &&
        g_ascii_strcasecmp(prefs->mints[i]->url, mint_url) == 0) {
      return TRUE;
    }
  }
  return FALSE;
}

/* ============== Cashu Proof ============== */

GnostrCashuProof *
gnostr_cashu_proof_new(void)
{
  return g_new0(GnostrCashuProof, 1);
}

void
gnostr_cashu_proof_free(GnostrCashuProof *proof)
{
  if (!proof) return;
  g_free(proof->id);
  g_free(proof->secret);
  g_free(proof->C);
  g_free(proof);
}

GnostrCashuProof **
gnostr_cashu_proofs_parse(const gchar *proofs_json,
                           gsize *out_count)
{
  if (!proofs_json || !*proofs_json) {
    if (out_count) *out_count = 0;
    return NULL;
  }

  json_error_t error;
  json_t *root = json_loads(proofs_json, 0, &error);
  if (!root) {
    g_warning("cashu_proofs: failed to parse JSON: %s", error.text);
    if (out_count) *out_count = 0;
    return NULL;
  }

  if (!json_is_array(root)) {
    g_debug("cashu_proofs: expected array");
    json_decref(root);
    if (out_count) *out_count = 0;
    return NULL;
  }

  gsize count = json_array_size(root);
  if (count == 0) {
    json_decref(root);
    if (out_count) *out_count = 0;
    return NULL;
  }

  GnostrCashuProof **proofs = g_new0(GnostrCashuProof *, count);
  gsize valid_count = 0;

  for (gsize i = 0; i < count; i++) {
    json_t *proof_obj = json_array_get(root, i);
    if (!json_is_object(proof_obj)) continue;

    GnostrCashuProof *proof = gnostr_cashu_proof_new();

    /* Parse amount */
    json_t *amount_val = json_object_get(proof_obj, "amount");
    if (amount_val && json_is_integer(amount_val)) {
      proof->amount = json_integer_value(amount_val);
    }

    /* Parse id (keyset ID) */
    json_t *id_val = json_object_get(proof_obj, "id");
    if (id_val && json_is_string(id_val)) {
      proof->id = g_strdup(json_string_value(id_val));
    }

    /* Parse secret */
    json_t *secret_val = json_object_get(proof_obj, "secret");
    if (secret_val && json_is_string(secret_val)) {
      proof->secret = g_strdup(json_string_value(secret_val));
    }

    /* Parse C (signature point) */
    json_t *C_val = json_object_get(proof_obj, "C");
    if (C_val && json_is_string(C_val)) {
      proof->C = g_strdup(json_string_value(C_val));
    }

    proofs[valid_count++] = proof;
  }

  json_decref(root);

  /* Resize if needed */
  if (valid_count < count) {
    proofs = g_renew(GnostrCashuProof *, proofs, valid_count);
  }

  if (out_count) *out_count = valid_count;

  g_debug("cashu_proofs: parsed %zu proofs", valid_count);
  return proofs;
}

void
gnostr_cashu_proofs_free(GnostrCashuProof **proofs, gsize count)
{
  if (!proofs) return;

  for (gsize i = 0; i < count; i++) {
    gnostr_cashu_proof_free(proofs[i]);
  }
  g_free(proofs);
}

gint64
gnostr_cashu_proofs_total_amount(GnostrCashuProof * const *proofs,
                                  gsize count)
{
  if (!proofs || count == 0) return 0;

  gint64 total = 0;
  for (gsize i = 0; i < count; i++) {
    if (proofs[i]) {
      total += proofs[i]->amount;
    }
  }
  return total;
}

/* ============== Nutzap ============== */

GnostrNutzap *
gnostr_nutzap_new(void)
{
  return g_new0(GnostrNutzap, 1);
}

void
gnostr_nutzap_free(GnostrNutzap *nutzap)
{
  if (!nutzap) return;

  g_free(nutzap->event_id);
  g_free(nutzap->sender_pubkey);
  g_free(nutzap->proofs_json);
  g_free(nutzap->mint_url);
  g_free(nutzap->zapped_event_id);
  g_free(nutzap->zapped_event_relay);
  g_free(nutzap->recipient_pubkey);
  g_free(nutzap->addressable_ref);

  /* Free parsed proofs */
  gnostr_cashu_proofs_free(nutzap->proofs, nutzap->proof_count);

  g_free(nutzap);
}

GnostrNutzap *
gnostr_nutzap_parse(const gchar *event_json)
{
  if (!event_json || !*event_json) return NULL;

  json_error_t error;
  json_t *root = json_loads(event_json, 0, &error);
  if (!root) {
    g_warning("nutzap: failed to parse JSON: %s", error.text);
    return NULL;
  }

  /* Verify kind 9321 */
  json_t *kind_val = json_object_get(root, "kind");
  if (!kind_val || json_integer_value(kind_val) != NIP61_KIND_NUTZAP) {
    g_debug("nutzap: wrong kind, expected %d", NIP61_KIND_NUTZAP);
    json_decref(root);
    return NULL;
  }

  GnostrNutzap *nutzap = gnostr_nutzap_new();

  /* Extract event ID */
  json_t *id_val = json_object_get(root, "id");
  if (id_val && json_is_string(id_val)) {
    nutzap->event_id = g_strdup(json_string_value(id_val));
  }

  /* Extract sender pubkey */
  json_t *pubkey_val = json_object_get(root, "pubkey");
  if (pubkey_val && json_is_string(pubkey_val)) {
    nutzap->sender_pubkey = g_strdup(json_string_value(pubkey_val));
  }

  /* Extract created_at */
  json_t *created_val = json_object_get(root, "created_at");
  if (created_val && json_is_integer(created_val)) {
    nutzap->created_at = json_integer_value(created_val);
  }

  /* Parse tags */
  json_t *tags = json_object_get(root, "tags");
  if (tags && json_is_array(tags)) {
    size_t i;
    json_t *tag;
    json_array_foreach(tags, i, tag) {
      if (!json_is_array(tag) || json_array_size(tag) < 2) continue;

      const char *tag_name = json_string_value(json_array_get(tag, 0));
      const char *tag_value = json_string_value(json_array_get(tag, 1));
      if (!tag_name || !tag_value) continue;

      if (g_strcmp0(tag_name, "proofs") == 0) {
        /* ["proofs", "<json-array>"] */
        nutzap->proofs_json = g_strdup(tag_value);

        /* Parse proofs for easy access */
        nutzap->proofs = gnostr_cashu_proofs_parse(tag_value, &nutzap->proof_count);

        /* Calculate total amount */
        nutzap->amount_sat = gnostr_cashu_proofs_total_amount(
            (GnostrCashuProof * const *)nutzap->proofs, nutzap->proof_count);

        g_debug("nutzap: parsed %zu proofs, total %lld sat",
                nutzap->proof_count, (long long)nutzap->amount_sat);

      } else if (g_strcmp0(tag_name, "u") == 0) {
        /* ["u", "<mint-url>"] */
        nutzap->mint_url = g_strdup(tag_value);

      } else if (g_strcmp0(tag_name, "e") == 0) {
        /* ["e", "<event-id>", "<relay>"] */
        nutzap->zapped_event_id = g_strdup(tag_value);
        if (json_array_size(tag) >= 3) {
          const char *relay = json_string_value(json_array_get(tag, 2));
          if (relay && *relay) {
            nutzap->zapped_event_relay = g_strdup(relay);
          }
        }

      } else if (g_strcmp0(tag_name, "p") == 0) {
        /* ["p", "<pubkey>"] */
        nutzap->recipient_pubkey = g_strdup(tag_value);

      } else if (g_strcmp0(tag_name, "a") == 0) {
        /* ["a", "<kind:pubkey:d-tag>"] */
        nutzap->addressable_ref = g_strdup(tag_value);
      }
    }
  }

  json_decref(root);

  /* Validate required fields */
  if (!nutzap->proofs_json || !nutzap->mint_url || !nutzap->recipient_pubkey) {
    g_debug("nutzap: missing required fields (proofs=%p, mint=%p, recipient=%p)",
            (void *)nutzap->proofs_json,
            (void *)nutzap->mint_url,
            (void *)nutzap->recipient_pubkey);
    gnostr_nutzap_free(nutzap);
    return NULL;
  }

  g_debug("nutzap: parsed event=%s amount=%lld sat mint=%.32s...",
          nutzap->event_id ? nutzap->event_id : "(none)",
          (long long)nutzap->amount_sat,
          nutzap->mint_url);

  return nutzap;
}

GPtrArray *
gnostr_nutzap_build_tags(const gchar *proofs_json,
                          const gchar *mint_url,
                          const gchar *event_id,
                          const gchar *event_relay,
                          const gchar *recipient_pubkey,
                          const gchar *addressable_ref)
{
  g_return_val_if_fail(proofs_json != NULL, NULL);
  g_return_val_if_fail(mint_url != NULL, NULL);
  g_return_val_if_fail(recipient_pubkey != NULL, NULL);

  GPtrArray *tags = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);

  /* proofs tag - required */
  {
    GPtrArray *tag = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(tag, g_strdup("proofs"));
    g_ptr_array_add(tag, g_strdup(proofs_json));
    g_ptr_array_add(tags, tag);
  }

  /* u tag (mint URL) - required */
  {
    GPtrArray *tag = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(tag, g_strdup("u"));
    g_ptr_array_add(tag, g_strdup(mint_url));
    g_ptr_array_add(tags, tag);
  }

  /* p tag (recipient) - required */
  {
    GPtrArray *tag = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(tag, g_strdup("p"));
    g_ptr_array_add(tag, g_strdup(recipient_pubkey));
    g_ptr_array_add(tags, tag);
  }

  /* e tag (event being zapped) - optional */
  if (event_id && *event_id) {
    GPtrArray *tag = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(tag, g_strdup("e"));
    g_ptr_array_add(tag, g_strdup(event_id));
    if (event_relay && *event_relay) {
      g_ptr_array_add(tag, g_strdup(event_relay));
    }
    g_ptr_array_add(tags, tag);
  }

  /* a tag (addressable event reference) - optional */
  if (addressable_ref && *addressable_ref) {
    GPtrArray *tag = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(tag, g_strdup("a"));
    g_ptr_array_add(tag, g_strdup(addressable_ref));
    g_ptr_array_add(tags, tag);
  }

  return tags;
}

gchar *
gnostr_nutzap_build_event_json(const gchar *proofs_json,
                                const gchar *mint_url,
                                const gchar *event_id,
                                const gchar *event_relay,
                                const gchar *recipient_pubkey,
                                const gchar *addressable_ref,
                                const gchar *sender_pubkey)
{
  g_return_val_if_fail(proofs_json != NULL, NULL);
  g_return_val_if_fail(mint_url != NULL, NULL);
  g_return_val_if_fail(recipient_pubkey != NULL, NULL);
  g_return_val_if_fail(sender_pubkey != NULL && strlen(sender_pubkey) == 64, NULL);

  json_t *event = json_object();

  /* Kind 9321 - nutzap */
  json_object_set_new(event, "kind", json_integer(NIP61_KIND_NUTZAP));

  /* Content - empty per spec */
  json_object_set_new(event, "content", json_string(""));

  /* Pubkey - sender */
  json_object_set_new(event, "pubkey", json_string(sender_pubkey));

  /* Created at */
  json_object_set_new(event, "created_at", json_integer((json_int_t)time(NULL)));

  /* Tags */
  json_t *tags = json_array();

  /* proofs tag - required */
  {
    json_t *tag = json_array();
    json_array_append_new(tag, json_string("proofs"));
    json_array_append_new(tag, json_string(proofs_json));
    json_array_append_new(tags, tag);
  }

  /* u tag (mint URL) - required */
  {
    json_t *tag = json_array();
    json_array_append_new(tag, json_string("u"));
    json_array_append_new(tag, json_string(mint_url));
    json_array_append_new(tags, tag);
  }

  /* p tag (recipient) - required */
  {
    json_t *tag = json_array();
    json_array_append_new(tag, json_string("p"));
    json_array_append_new(tag, json_string(recipient_pubkey));
    json_array_append_new(tags, tag);
  }

  /* e tag (event being zapped) - optional */
  if (event_id && *event_id) {
    json_t *tag = json_array();
    json_array_append_new(tag, json_string("e"));
    json_array_append_new(tag, json_string(event_id));
    if (event_relay && *event_relay) {
      json_array_append_new(tag, json_string(event_relay));
    }
    json_array_append_new(tags, tag);
  }

  /* a tag (addressable event reference) - optional */
  if (addressable_ref && *addressable_ref) {
    json_t *tag = json_array();
    json_array_append_new(tag, json_string("a"));
    json_array_append_new(tag, json_string(addressable_ref));
    json_array_append_new(tags, tag);
  }

  json_object_set_new(event, "tags", tags);

  gchar *result = json_dumps(event, JSON_COMPACT);
  json_decref(event);

  return result;
}

/* ============== Utility Functions ============== */

gchar *
gnostr_nutzap_format_amount(gint64 amount_sat)
{
  if (amount_sat >= 1000000) {
    return g_strdup_printf("%.1fM sats", amount_sat / 1000000.0);
  } else if (amount_sat >= 10000) {
    return g_strdup_printf("%.1fK sats", amount_sat / 1000.0);
  } else if (amount_sat >= 1000) {
    return g_strdup_printf("%'lld sats", (long long)amount_sat);
  } else {
    return g_strdup_printf("%lld sats", (long long)amount_sat);
  }
}

gboolean
gnostr_nutzap_is_valid_mint_url(const gchar *url)
{
  if (!url || !*url) return FALSE;

  /* Must be https (or http for localhost in development) */
  if (!g_str_has_prefix(url, "https://") &&
      !g_str_has_prefix(url, "http://localhost") &&
      !g_str_has_prefix(url, "http://127.0.0.1")) {
    return FALSE;
  }

  /* Reasonable length check */
  gsize len = strlen(url);
  if (len < 10 || len > 2048) {
    return FALSE;
  }

  return TRUE;
}
