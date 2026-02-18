/**
 * NIP-61: Nutzaps (Ecash Zaps) Implementation
 *
 * Implements nutzap preferences parsing, nutzap event parsing,
 * and event building for Cashu ecash zaps on Nostr.
 */

#define G_LOG_DOMAIN "nip61-nutzaps"

#include "nip61_nutzaps.h"
#include <nostr-gobject-1.0/nostr_json.h>
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

/* Callback context for parsing nutzap prefs tags */
typedef struct {
  GPtrArray *mints_arr;
  GPtrArray *relays_arr;
  gboolean require_p2pk;
} NutzapPrefsParseCtx;

static gboolean
nutzap_prefs_tag_callback(gsize index, const gchar *element_json, gpointer user_data)
{
  (void)index;
  NutzapPrefsParseCtx *ctx = user_data;

  char *tag_name = NULL;
  tag_name = gnostr_json_get_array_string(element_json, NULL, 0, NULL);
  if (!tag_name) {
    return TRUE;
  }

  if (g_strcmp0(tag_name, "mint") == 0) {
    /* ["mint", "<url>", "<unit>", "<optional-pubkey>"] */
    char *url = NULL;
    char *unit = NULL;
    char *pubkey = NULL;

    if ((url = gnostr_json_get_array_string(element_json, NULL, 1, NULL)) != NULL && url &&
        (unit = gnostr_json_get_array_string(element_json, NULL, 2, NULL)) != NULL && unit) {
      pubkey = gnostr_json_get_array_string(element_json, NULL, 3, NULL);

      GnostrNutzapMint *mint = gnostr_nutzap_mint_new_full(url, unit, pubkey);
      g_ptr_array_add(ctx->mints_arr, mint);
      g_debug("nutzap_prefs: parsed mint url=%s unit=%s", url, unit);
    }

    free(url);
    free(unit);
    free(pubkey);
  } else if (g_strcmp0(tag_name, "relay") == 0) {
    /* ["relay", "<url>"] */
    char *relay_url = NULL;
    if ((relay_url = gnostr_json_get_array_string(element_json, NULL, 1, NULL)) != NULL &&
        relay_url && *relay_url) {
      g_ptr_array_add(ctx->relays_arr, g_strdup(relay_url));
      g_debug("nutzap_prefs: parsed relay=%s", relay_url);
      free(relay_url);
    }
  } else if (g_strcmp0(tag_name, "p2pk") == 0) {
    /* ["p2pk"] - presence indicates requirement */
    ctx->require_p2pk = TRUE;
    g_debug("nutzap_prefs: p2pk required");
  }

  free(tag_name);
  return TRUE;
}

GnostrNutzapPrefs *
gnostr_nutzap_prefs_parse(const gchar *event_json)
{
  if (!event_json || !*event_json) return NULL;

  if (!gnostr_json_is_valid(event_json)) {
    g_warning("nutzap_prefs: failed to parse JSON");
    return NULL;
  }

  /* Verify kind 10019 */
  int kind = gnostr_json_get_int(event_json, "kind", NULL);
  if (kind != NIP61_KIND_NUTZAP_PREFS) {
    g_debug("nutzap_prefs: wrong kind, expected %d", NIP61_KIND_NUTZAP_PREFS);
    return NULL;
  }

  GnostrNutzapPrefs *prefs = gnostr_nutzap_prefs_new();

  /* Temporary arrays for collecting mints and relays */
  NutzapPrefsParseCtx ctx = {
    .mints_arr = g_ptr_array_new(),
    .relays_arr = g_ptr_array_new_with_free_func(g_free),
    .require_p2pk = FALSE
  };

  /* Parse tags */
  char *tags_json = NULL;
  tags_json = gnostr_json_get_raw(event_json, "tags", NULL);
  if (tags_json) {
    gnostr_json_array_foreach_root(tags_json, nutzap_prefs_tag_callback, &ctx);
    free(tags_json);
  }

  prefs->require_p2pk = ctx.require_p2pk;

  /* Transfer mints to prefs */
  prefs->mint_count = ctx.mints_arr->len;
  if (prefs->mint_count > 0) {
    prefs->mints = g_new(GnostrNutzapMint *, prefs->mint_count);
    for (gsize i = 0; i < prefs->mint_count; i++) {
      prefs->mints[i] = g_ptr_array_index(ctx.mints_arr, i);
    }
  }
  g_ptr_array_free(ctx.mints_arr, TRUE);

  /* Transfer relays to prefs */
  prefs->relay_count = ctx.relays_arr->len;
  if (prefs->relay_count > 0) {
    prefs->relays = g_new(gchar *, prefs->relay_count);
    for (gsize i = 0; i < prefs->relay_count; i++) {
      prefs->relays[i] = g_ptr_array_index(ctx.relays_arr, i);
    }
  }
  /* Don't free the strings, just the array */
  g_ptr_array_free(ctx.relays_arr, TRUE);

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

  g_autoptr(GNostrJsonBuilder) builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);

  /* Kind 10019 - nutzap preferences */
  gnostr_json_builder_set_key(builder, "kind");
  gnostr_json_builder_add_int(builder, NIP61_KIND_NUTZAP_PREFS);

  /* Content - empty per spec */
  gnostr_json_builder_set_key(builder, "content");
  gnostr_json_builder_add_string(builder, "");

  /* Pubkey */
  gnostr_json_builder_set_key(builder, "pubkey");
  gnostr_json_builder_add_string(builder, pubkey);

  /* Created at */
  gnostr_json_builder_set_key(builder, "created_at");
  gnostr_json_builder_add_int64(builder, (int64_t)time(NULL));

  /* Tags */
  gnostr_json_builder_set_key(builder, "tags");
  gnostr_json_builder_begin_array(builder);

  /* Add mint tags */
  for (gsize i = 0; i < prefs->mint_count; i++) {
    GnostrNutzapMint *mint = prefs->mints[i];

    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "mint");
    gnostr_json_builder_add_string(builder, mint->url ? mint->url : "");
    gnostr_json_builder_add_string(builder, mint->unit ? mint->unit : "sat");

    if (mint->pubkey && *mint->pubkey) {
      gnostr_json_builder_add_string(builder, mint->pubkey);
    }

    gnostr_json_builder_end_array(builder);
  }

  /* Add relay tags */
  for (gsize i = 0; i < prefs->relay_count; i++) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "relay");
    gnostr_json_builder_add_string(builder, prefs->relays[i]);
    gnostr_json_builder_end_array(builder);
  }

  /* Add p2pk tag if required */
  if (prefs->require_p2pk) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "p2pk");
    gnostr_json_builder_end_array(builder);
  }

  gnostr_json_builder_end_array(builder);  /* tags */
  gnostr_json_builder_end_object(builder);

  char *result = gnostr_json_builder_finish(builder);

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

/* Callback context for parsing cashu proofs */
typedef struct {
  GPtrArray *proofs_arr;
} CashuProofsParseCtx;

static gboolean
cashu_proof_callback(gsize index, const gchar *element_json, gpointer user_data)
{
  (void)index;
  CashuProofsParseCtx *ctx = user_data;

  if (!gnostr_json_is_object_str(element_json)) {
    return TRUE;
  }

  GnostrCashuProof *proof = gnostr_cashu_proof_new();

  /* Parse amount */
  int64_t amount = 0;
  if ((amount = gnostr_json_get_int64(element_json, "amount", NULL), TRUE)) {
    proof->amount = amount;
  }

  /* Parse id (keyset ID) */
  char *id_val = NULL;
  id_val = gnostr_json_get_string(element_json, "id", NULL);
  if (id_val) {
    proof->id = g_strdup(id_val);
    free(id_val);
  }

  /* Parse secret */
  char *secret_val = NULL;
  secret_val = gnostr_json_get_string(element_json, "secret", NULL);
  if (secret_val) {
    proof->secret = g_strdup(secret_val);
    free(secret_val);
  }

  /* Parse C (signature point) */
  char *C_val = NULL;
  C_val = gnostr_json_get_string(element_json, "C", NULL);
  if (C_val) {
    proof->C = g_strdup(C_val);
    free(C_val);
  }

  g_ptr_array_add(ctx->proofs_arr, proof);
  return TRUE;
}

GnostrCashuProof **
gnostr_cashu_proofs_parse(const gchar *proofs_json,
                           gsize *out_count)
{
  if (!proofs_json || !*proofs_json) {
    if (out_count) *out_count = 0;
    return NULL;
  }

  if (!gnostr_json_is_valid(proofs_json) || !gnostr_json_is_array_str(proofs_json)) {
    g_warning("cashu_proofs: failed to parse JSON or not an array");
    if (out_count) *out_count = 0;
    return NULL;
  }

  CashuProofsParseCtx ctx = {
    .proofs_arr = g_ptr_array_new()
  };

  gnostr_json_array_foreach_root(proofs_json, cashu_proof_callback, &ctx);

  gsize valid_count = ctx.proofs_arr->len;
  if (valid_count == 0) {
    g_ptr_array_free(ctx.proofs_arr, TRUE);
    if (out_count) *out_count = 0;
    return NULL;
  }

  GnostrCashuProof **proofs = g_new0(GnostrCashuProof *, valid_count);
  for (gsize i = 0; i < valid_count; i++) {
    proofs[i] = g_ptr_array_index(ctx.proofs_arr, i);
  }
  g_ptr_array_free(ctx.proofs_arr, TRUE);

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

/* Callback context for parsing nutzap tags */
typedef struct {
  GnostrNutzap *nutzap;
} NutzapParseCtx;

static gboolean
nutzap_tag_callback(gsize index, const gchar *element_json, gpointer user_data)
{
  (void)index;
  NutzapParseCtx *ctx = user_data;

  char *tag_name = NULL;
  char *tag_value = NULL;

  tag_name = gnostr_json_get_array_string(element_json, NULL, 0, NULL);
  if (!tag_name) {
    return TRUE;
  }

  tag_value = gnostr_json_get_array_string(element_json, NULL, 1, NULL);
  if (!tag_value) {
    free(tag_name);
    return TRUE;
  }

  if (g_strcmp0(tag_name, "proofs") == 0) {
    /* ["proofs", "<json-array>"] */
    ctx->nutzap->proofs_json = g_strdup(tag_value);

    /* Parse proofs for easy access */
    ctx->nutzap->proofs = gnostr_cashu_proofs_parse(tag_value, &ctx->nutzap->proof_count);

    /* Calculate total amount */
    ctx->nutzap->amount_sat = gnostr_cashu_proofs_total_amount(
        (GnostrCashuProof * const *)ctx->nutzap->proofs, ctx->nutzap->proof_count);

    g_debug("nutzap: parsed %zu proofs, total %lld sat",
            ctx->nutzap->proof_count, (long long)ctx->nutzap->amount_sat);

  } else if (g_strcmp0(tag_name, "u") == 0) {
    /* ["u", "<mint-url>"] */
    ctx->nutzap->mint_url = g_strdup(tag_value);

  } else if (g_strcmp0(tag_name, "e") == 0) {
    /* ["e", "<event-id>", "<relay>"] */
    ctx->nutzap->zapped_event_id = g_strdup(tag_value);
    char *relay = NULL;
    if ((relay = gnostr_json_get_array_string(element_json, NULL, 2, NULL)) != NULL && relay && *relay) {
      ctx->nutzap->zapped_event_relay = g_strdup(relay);
      free(relay);
    }

  } else if (g_strcmp0(tag_name, "p") == 0) {
    /* ["p", "<pubkey>"] */
    ctx->nutzap->recipient_pubkey = g_strdup(tag_value);

  } else if (g_strcmp0(tag_name, "a") == 0) {
    /* ["a", "<kind:pubkey:d-tag>"] */
    ctx->nutzap->addressable_ref = g_strdup(tag_value);
  }

  free(tag_name);
  free(tag_value);
  return TRUE;
}

GnostrNutzap *
gnostr_nutzap_parse(const gchar *event_json)
{
  if (!event_json || !*event_json) return NULL;

  if (!gnostr_json_is_valid(event_json)) {
    g_warning("nutzap: failed to parse JSON");
    return NULL;
  }

  /* Verify kind 9321 */
  int kind = gnostr_json_get_int(event_json, "kind", NULL);
  if (kind != NIP61_KIND_NUTZAP) {
    g_debug("nutzap: wrong kind, expected %d", NIP61_KIND_NUTZAP);
    return NULL;
  }

  GnostrNutzap *nutzap = gnostr_nutzap_new();

  /* Extract event ID */
  char *id_val = NULL;
  id_val = gnostr_json_get_string(event_json, "id", NULL);
  if (id_val) {
    nutzap->event_id = g_strdup(id_val);
    free(id_val);
  }

  /* Extract sender pubkey */
  char *pubkey_val = NULL;
  pubkey_val = gnostr_json_get_string(event_json, "pubkey", NULL);
  if (pubkey_val) {
    nutzap->sender_pubkey = g_strdup(pubkey_val);
    free(pubkey_val);
  }

  /* Extract created_at */
  int64_t created_at = 0;
  if ((created_at = gnostr_json_get_int64(event_json, "created_at", NULL), TRUE)) {
    nutzap->created_at = created_at;
  }

  /* Parse tags */
  char *tags_json = NULL;
  tags_json = gnostr_json_get_raw(event_json, "tags", NULL);
  if (tags_json) {
    NutzapParseCtx ctx = { .nutzap = nutzap };
    gnostr_json_array_foreach_root(tags_json, nutzap_tag_callback, &ctx);
    free(tags_json);
  }

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

  g_autoptr(GNostrJsonBuilder) builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);

  /* Kind 9321 - nutzap */
  gnostr_json_builder_set_key(builder, "kind");
  gnostr_json_builder_add_int(builder, NIP61_KIND_NUTZAP);

  /* Content - empty per spec */
  gnostr_json_builder_set_key(builder, "content");
  gnostr_json_builder_add_string(builder, "");

  /* Pubkey - sender */
  gnostr_json_builder_set_key(builder, "pubkey");
  gnostr_json_builder_add_string(builder, sender_pubkey);

  /* Created at */
  gnostr_json_builder_set_key(builder, "created_at");
  gnostr_json_builder_add_int64(builder, (int64_t)time(NULL));

  /* Tags */
  gnostr_json_builder_set_key(builder, "tags");
  gnostr_json_builder_begin_array(builder);

  /* proofs tag - required */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "proofs");
  gnostr_json_builder_add_string(builder, proofs_json);
  gnostr_json_builder_end_array(builder);

  /* u tag (mint URL) - required */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "u");
  gnostr_json_builder_add_string(builder, mint_url);
  gnostr_json_builder_end_array(builder);

  /* p tag (recipient) - required */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "p");
  gnostr_json_builder_add_string(builder, recipient_pubkey);
  gnostr_json_builder_end_array(builder);

  /* e tag (event being zapped) - optional */
  if (event_id && *event_id) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "e");
    gnostr_json_builder_add_string(builder, event_id);
    if (event_relay && *event_relay) {
      gnostr_json_builder_add_string(builder, event_relay);
    }
    gnostr_json_builder_end_array(builder);
  }

  /* a tag (addressable event reference) - optional */
  if (addressable_ref && *addressable_ref) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "a");
    gnostr_json_builder_add_string(builder, addressable_ref);
    gnostr_json_builder_end_array(builder);
  }

  gnostr_json_builder_end_array(builder);  /* tags */
  gnostr_json_builder_end_object(builder);

  char *result = gnostr_json_builder_finish(builder);

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
