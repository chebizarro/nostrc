/**
 * nip87_ecash.c - NIP-87 Ecash Mint Discovery implementation for GNostr
 *
 * Implements mint recommendation parsing and tag building for:
 *   - Kind 38000: Mint recommendation
 */

#define G_LOG_DOMAIN "nip87-ecash"

#include "nip87_ecash.h"
#include "json.h"
#include <string.h>
#include <ctype.h>

/* ============== Memory Management ============== */

GnostrEcashMint *
gnostr_ecash_mint_new(void)
{
  GnostrEcashMint *mint = g_new0(GnostrEcashMint, 1);
  mint->network = GNOSTR_ECASH_NETWORK_UNKNOWN;
  mint->units = NULL;
  mint->unit_count = 0;
  mint->tags = NULL;
  mint->tag_count = 0;
  return mint;
}

void
gnostr_ecash_mint_free(GnostrEcashMint *mint)
{
  if (!mint) return;

  g_free(mint->event_id_hex);
  g_free(mint->pubkey);
  g_free(mint->mint_url);
  g_free(mint->d_tag);

  /* Free units array */
  if (mint->units) {
    for (gsize i = 0; i < mint->unit_count; i++) {
      g_free(mint->units[i]);
    }
    g_free(mint->units);
  }

  /* Free tags array */
  if (mint->tags) {
    for (gsize i = 0; i < mint->tag_count; i++) {
      g_free(mint->tags[i]);
    }
    g_free(mint->tags);
  }

  g_free(mint);
}

GnostrEcashMint *
gnostr_ecash_mint_copy(const GnostrEcashMint *mint)
{
  if (!mint) return NULL;

  GnostrEcashMint *copy = gnostr_ecash_mint_new();

  copy->event_id_hex = g_strdup(mint->event_id_hex);
  copy->pubkey = g_strdup(mint->pubkey);
  copy->mint_url = g_strdup(mint->mint_url);
  copy->d_tag = g_strdup(mint->d_tag);
  copy->network = mint->network;
  copy->created_at = mint->created_at;
  copy->cached_at = mint->cached_at;

  /* Copy units */
  if (mint->units && mint->unit_count > 0) {
    copy->units = g_new0(gchar *, mint->unit_count);
    copy->unit_count = mint->unit_count;
    for (gsize i = 0; i < mint->unit_count; i++) {
      copy->units[i] = g_strdup(mint->units[i]);
    }
  }

  /* Copy tags */
  if (mint->tags && mint->tag_count > 0) {
    copy->tags = g_new0(gchar *, mint->tag_count);
    copy->tag_count = mint->tag_count;
    for (gsize i = 0; i < mint->tag_count; i++) {
      copy->tags[i] = g_strdup(mint->tags[i]);
    }
  }

  return copy;
}

/* ============== Network Parsing ============== */

GnostrEcashNetwork
gnostr_ecash_parse_network(const gchar *network_str)
{
  if (!network_str || !*network_str) {
    return GNOSTR_ECASH_NETWORK_UNKNOWN;
  }

  if (g_ascii_strcasecmp(network_str, "mainnet") == 0) {
    return GNOSTR_ECASH_NETWORK_MAINNET;
  } else if (g_ascii_strcasecmp(network_str, "testnet") == 0) {
    return GNOSTR_ECASH_NETWORK_TESTNET;
  } else if (g_ascii_strcasecmp(network_str, "signet") == 0) {
    return GNOSTR_ECASH_NETWORK_SIGNET;
  }

  return GNOSTR_ECASH_NETWORK_UNKNOWN;
}

const gchar *
gnostr_ecash_network_to_string(GnostrEcashNetwork network)
{
  switch (network) {
    case GNOSTR_ECASH_NETWORK_MAINNET:
      return "mainnet";
    case GNOSTR_ECASH_NETWORK_TESTNET:
      return "testnet";
    case GNOSTR_ECASH_NETWORK_SIGNET:
      return "signet";
    case GNOSTR_ECASH_NETWORK_UNKNOWN:
    default:
      return "unknown";
  }
}

/* ============== URL Validation ============== */

gboolean
gnostr_ecash_validate_mint_url(const gchar *url)
{
  if (!url || !*url) {
    return FALSE;
  }

  /* Must use https:// */
  if (!g_str_has_prefix(url, "https://")) {
    g_debug("ecash: mint URL must use https://: %s", url);
    return FALSE;
  }

  /* Check for valid host after https:// */
  const gchar *host_start = url + 8; /* After "https://" */
  if (!*host_start || *host_start == '/' || *host_start == ':') {
    g_debug("ecash: mint URL has no host: %s", url);
    return FALSE;
  }

  /* Find end of host (first / or : after host) */
  const gchar *host_end = host_start;
  while (*host_end && *host_end != '/' && *host_end != ':') {
    host_end++;
  }

  /* Host must have at least one character */
  if (host_end == host_start) {
    g_debug("ecash: mint URL has empty host: %s", url);
    return FALSE;
  }

  /* Check for valid host characters */
  for (const gchar *p = host_start; p < host_end; p++) {
    char c = *p;
    if (!g_ascii_isalnum(c) && c != '.' && c != '-') {
      g_debug("ecash: mint URL has invalid host character '%c': %s", c, url);
      return FALSE;
    }
  }

  return TRUE;
}

gchar *
gnostr_ecash_normalize_mint_url(const gchar *url)
{
  if (!url || !*url) {
    return NULL;
  }

  /* Validate first */
  if (!gnostr_ecash_validate_mint_url(url)) {
    return NULL;
  }

  /* Convert to lowercase */
  gchar *normalized = g_ascii_strdown(url, -1);

  /* Remove trailing slashes */
  gsize len = strlen(normalized);
  while (len > 0 && normalized[len - 1] == '/') {
    normalized[len - 1] = '\0';
    len--;
  }

  return normalized;
}

/* ============== Unit Validation ============== */

/* Known valid currency units */
static const gchar *valid_units[] = {
  "sat", "msat",                          /* Bitcoin satoshis */
  "usd", "eur", "gbp", "cad", "aud",      /* Major fiat */
  "jpy", "chf", "cny", "hkd", "sgd",      /* Asian/other */
  "nzd", "sek", "nok", "dkk", "krw",      /* More fiat */
  "btc",                                   /* Bitcoin */
  NULL
};

gboolean
gnostr_ecash_is_valid_unit(const gchar *unit)
{
  if (!unit || !*unit) {
    return FALSE;
  }

  /* Convert to lowercase for comparison */
  gchar *lower = g_ascii_strdown(unit, -1);

  gboolean valid = FALSE;
  for (const gchar **p = valid_units; *p != NULL; p++) {
    if (g_strcmp0(lower, *p) == 0) {
      valid = TRUE;
      break;
    }
  }

  g_free(lower);
  return valid;
}

const gchar *
gnostr_ecash_format_unit(const gchar *unit)
{
  if (!unit || !*unit) {
    return "Unknown";
  }

  if (g_ascii_strcasecmp(unit, "sat") == 0) return "Satoshis";
  if (g_ascii_strcasecmp(unit, "msat") == 0) return "Millisatoshis";
  if (g_ascii_strcasecmp(unit, "btc") == 0) return "Bitcoin";
  if (g_ascii_strcasecmp(unit, "usd") == 0) return "US Dollar";
  if (g_ascii_strcasecmp(unit, "eur") == 0) return "Euro";
  if (g_ascii_strcasecmp(unit, "gbp") == 0) return "British Pound";
  if (g_ascii_strcasecmp(unit, "cad") == 0) return "Canadian Dollar";
  if (g_ascii_strcasecmp(unit, "aud") == 0) return "Australian Dollar";
  if (g_ascii_strcasecmp(unit, "jpy") == 0) return "Japanese Yen";
  if (g_ascii_strcasecmp(unit, "chf") == 0) return "Swiss Franc";
  if (g_ascii_strcasecmp(unit, "cny") == 0) return "Chinese Yuan";

  /* Return uppercase version for unknown units */
  return unit;
}

/* ============== Mint Helpers ============== */

gboolean
gnostr_ecash_mint_has_unit(const GnostrEcashMint *mint, const gchar *unit)
{
  if (!mint || !unit || !mint->units) {
    return FALSE;
  }

  for (gsize i = 0; i < mint->unit_count; i++) {
    if (g_ascii_strcasecmp(mint->units[i], unit) == 0) {
      return TRUE;
    }
  }

  return FALSE;
}

gboolean
gnostr_ecash_mint_has_tag(const GnostrEcashMint *mint, const gchar *tag)
{
  if (!mint || !tag || !mint->tags) {
    return FALSE;
  }

  for (gsize i = 0; i < mint->tag_count; i++) {
    if (g_ascii_strcasecmp(mint->tags[i], tag) == 0) {
      return TRUE;
    }
  }

  return FALSE;
}

void
gnostr_ecash_mint_add_unit(GnostrEcashMint *mint, const gchar *unit)
{
  if (!mint || !unit || !*unit) {
    return;
  }

  /* Check if already present */
  if (gnostr_ecash_mint_has_unit(mint, unit)) {
    return;
  }

  /* Grow the array */
  mint->unit_count++;
  mint->units = g_realloc(mint->units, mint->unit_count * sizeof(gchar *));
  mint->units[mint->unit_count - 1] = g_strdup(unit);
}

void
gnostr_ecash_mint_add_tag(GnostrEcashMint *mint, const gchar *tag)
{
  if (!mint || !tag || !*tag) {
    return;
  }

  /* Check if already present */
  if (gnostr_ecash_mint_has_tag(mint, tag)) {
    return;
  }

  /* Grow the array */
  mint->tag_count++;
  mint->tags = g_realloc(mint->tags, mint->tag_count * sizeof(gchar *));
  mint->tags[mint->tag_count - 1] = g_strdup(tag);
}

/* ============== Tag Parsing ============== */

/* Callback context for parsing mint tags */
typedef struct {
  GnostrEcashMint *mint;
  gboolean found_url;
} EcashMintParseCtx;

static bool
ecash_mint_tag_callback(size_t index, const char *element_json, void *user_data)
{
  (void)index;
  EcashMintParseCtx *ctx = user_data;

  char *tag_name = NULL;
  char *tag_value = NULL;

  if (nostr_json_get_array_string(element_json, NULL, 0, &tag_name) != 0 || !tag_name) {
    return true;
  }

  if (nostr_json_get_array_string(element_json, NULL, 1, &tag_value) != 0 || !tag_value) {
    free(tag_name);
    return true;
  }

  if (g_strcmp0(tag_name, "d") == 0) {
    /* d tag - unique identifier (mint URL) */
    g_free(ctx->mint->d_tag);
    ctx->mint->d_tag = g_strdup(tag_value);

    /* Use d tag as mint URL if not already set */
    if (!ctx->mint->mint_url && gnostr_ecash_validate_mint_url(tag_value)) {
      ctx->mint->mint_url = gnostr_ecash_normalize_mint_url(tag_value);
      ctx->found_url = TRUE;
    }
  } else if (g_strcmp0(tag_name, "u") == 0) {
    /* u tag - mint URL (preferred over d tag) */
    if (gnostr_ecash_validate_mint_url(tag_value)) {
      g_free(ctx->mint->mint_url);
      ctx->mint->mint_url = gnostr_ecash_normalize_mint_url(tag_value);
      ctx->found_url = TRUE;
    } else {
      g_debug("ecash: invalid mint URL in 'u' tag: %s", tag_value);
    }
  } else if (g_strcmp0(tag_name, "network") == 0) {
    /* network tag - bitcoin network type */
    ctx->mint->network = gnostr_ecash_parse_network(tag_value);
  } else if (g_strcmp0(tag_name, "k") == 0) {
    /* k tag - currency unit */
    gnostr_ecash_mint_add_unit(ctx->mint, tag_value);
  } else if (g_strcmp0(tag_name, "t") == 0) {
    /* t tag - category/tag */
    gnostr_ecash_mint_add_tag(ctx->mint, tag_value);
  }

  free(tag_name);
  free(tag_value);
  return true;
}

gboolean
gnostr_ecash_mint_parse_tags(GnostrEcashMint *mint, const gchar *tags_json)
{
  if (!mint || !tags_json || !*tags_json) {
    return FALSE;
  }

  if (!nostr_json_is_valid(tags_json) || !nostr_json_is_array_str(tags_json)) {
    g_warning("ecash: failed to parse tags JSON or not an array");
    return FALSE;
  }

  EcashMintParseCtx ctx = { .mint = mint, .found_url = FALSE };
  nostr_json_array_foreach_root(tags_json, ecash_mint_tag_callback, &ctx);

  /* Must have a valid mint URL */
  if (!ctx.found_url && !mint->mint_url) {
    g_debug("ecash: no valid mint URL found in tags");
    return FALSE;
  }

  return TRUE;
}

/* ============== Event Parsing ============== */

GnostrEcashMint *
gnostr_ecash_mint_parse_event(const gchar *event_json)
{
  if (!event_json || !*event_json) {
    return NULL;
  }

  if (!nostr_json_is_valid(event_json)) {
    g_warning("ecash: failed to parse event JSON");
    return NULL;
  }

  /* Verify kind */
  int kind = 0;
  if (nostr_json_get_int(event_json, "kind", &kind) != 0 ||
      kind != NIP87_KIND_MINT_RECOMMENDATION) {
    g_debug("ecash: event is not kind 38000");
    return NULL;
  }

  GnostrEcashMint *mint = gnostr_ecash_mint_new();

  /* Extract event ID */
  char *id_val = NULL;
  if (nostr_json_get_string(event_json, "id", &id_val) == 0 && id_val) {
    mint->event_id_hex = g_strdup(id_val);
    free(id_val);
  }

  /* Extract pubkey */
  char *pubkey_val = NULL;
  if (nostr_json_get_string(event_json, "pubkey", &pubkey_val) == 0 && pubkey_val) {
    mint->pubkey = g_strdup(pubkey_val);
    free(pubkey_val);
  }

  /* Extract created_at */
  int64_t created_at = 0;
  if (nostr_json_get_int64(event_json, "created_at", &created_at) == 0) {
    mint->created_at = created_at;
  }

  /* Parse tags */
  char *tags_str = NULL;
  if (nostr_json_get_raw(event_json, "tags", &tags_str) == 0 && tags_str) {
    if (!gnostr_ecash_mint_parse_tags(mint, tags_str)) {
      g_debug("ecash: failed to parse required tags");
      free(tags_str);
      gnostr_ecash_mint_free(mint);
      return NULL;
    }
    free(tags_str);
  } else {
    g_debug("ecash: event has no tags array");
    gnostr_ecash_mint_free(mint);
    return NULL;
  }

  g_debug("ecash: parsed mint recommendation for %s (network=%s, %zu units, %zu tags)",
          mint->mint_url ? mint->mint_url : "(unknown)",
          gnostr_ecash_network_to_string(mint->network),
          mint->unit_count,
          mint->tag_count);

  return mint;
}

/* ============== Tag Building ============== */

gchar *
gnostr_ecash_build_recommendation_tags(const GnostrEcashMint *mint)
{
  if (!mint || !mint->mint_url) {
    return NULL;
  }

  NostrJsonBuilder *builder = nostr_json_builder_new();
  nostr_json_builder_begin_array(builder);

  /* d tag - unique identifier (mint URL) */
  nostr_json_builder_begin_array(builder);
  nostr_json_builder_add_string(builder, "d");
  nostr_json_builder_add_string(builder, mint->d_tag ? mint->d_tag : mint->mint_url);
  nostr_json_builder_end_array(builder);

  /* u tag - mint URL */
  nostr_json_builder_begin_array(builder);
  nostr_json_builder_add_string(builder, "u");
  nostr_json_builder_add_string(builder, mint->mint_url);
  nostr_json_builder_end_array(builder);

  /* network tag (if not unknown) */
  if (mint->network != GNOSTR_ECASH_NETWORK_UNKNOWN) {
    nostr_json_builder_begin_array(builder);
    nostr_json_builder_add_string(builder, "network");
    nostr_json_builder_add_string(builder, gnostr_ecash_network_to_string(mint->network));
    nostr_json_builder_end_array(builder);
  }

  /* k tags - currency units */
  for (gsize i = 0; i < mint->unit_count; i++) {
    nostr_json_builder_begin_array(builder);
    nostr_json_builder_add_string(builder, "k");
    nostr_json_builder_add_string(builder, mint->units[i]);
    nostr_json_builder_end_array(builder);
  }

  /* t tags - categories/tags */
  for (gsize i = 0; i < mint->tag_count; i++) {
    nostr_json_builder_begin_array(builder);
    nostr_json_builder_add_string(builder, "t");
    nostr_json_builder_add_string(builder, mint->tags[i]);
    nostr_json_builder_end_array(builder);
  }

  nostr_json_builder_end_array(builder);

  char *result = nostr_json_builder_finish(builder);
  nostr_json_builder_free(builder);

  return result;
}

GPtrArray *
gnostr_ecash_build_recommendation_tags_array(const GnostrEcashMint *mint)
{
  if (!mint || !mint->mint_url) {
    return NULL;
  }

  GPtrArray *tags = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);

  /* Helper to create a tag array */
  #define ADD_TAG(name, value) do { \
    GPtrArray *tag = g_ptr_array_new_with_free_func(g_free); \
    g_ptr_array_add(tag, g_strdup(name)); \
    g_ptr_array_add(tag, g_strdup(value)); \
    g_ptr_array_add(tags, tag); \
  } while (0)

  /* d tag - unique identifier (mint URL) */
  ADD_TAG("d", mint->d_tag ? mint->d_tag : mint->mint_url);

  /* u tag - mint URL */
  ADD_TAG("u", mint->mint_url);

  /* network tag (if not unknown) */
  if (mint->network != GNOSTR_ECASH_NETWORK_UNKNOWN) {
    ADD_TAG("network", gnostr_ecash_network_to_string(mint->network));
  }

  /* k tags - currency units */
  for (gsize i = 0; i < mint->unit_count; i++) {
    ADD_TAG("k", mint->units[i]);
  }

  /* t tags - categories/tags */
  for (gsize i = 0; i < mint->tag_count; i++) {
    ADD_TAG("t", mint->tags[i]);
  }

  #undef ADD_TAG

  return tags;
}

/* ============== Filter Building ============== */

gchar *
gnostr_ecash_build_mint_filter(const gchar **pubkeys,
                                gsize n_pubkeys,
                                gint limit)
{
  NostrJsonBuilder *builder = nostr_json_builder_new();
  nostr_json_builder_begin_object(builder);

  /* Set kind */
  nostr_json_builder_set_key(builder, "kinds");
  nostr_json_builder_begin_array(builder);
  nostr_json_builder_add_int(builder, NIP87_KIND_MINT_RECOMMENDATION);
  nostr_json_builder_end_array(builder);

  /* Set authors if provided */
  if (pubkeys && n_pubkeys > 0) {
    nostr_json_builder_set_key(builder, "authors");
    nostr_json_builder_begin_array(builder);
    for (gsize i = 0; i < n_pubkeys; i++) {
      if (pubkeys[i]) {
        nostr_json_builder_add_string(builder, pubkeys[i]);
      }
    }
    nostr_json_builder_end_array(builder);
  }

  /* Set limit */
  nostr_json_builder_set_key(builder, "limit");
  nostr_json_builder_add_int(builder, limit > 0 ? limit : 100);

  nostr_json_builder_end_object(builder);

  char *result = nostr_json_builder_finish(builder);
  nostr_json_builder_free(builder);

  return result;
}
