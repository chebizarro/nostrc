/* nd-uri.c — NIP-21 parsing / canonical encoding. See nd-uri.h. */
#include "nd-uri.h"
#include "nd-error.h"

#include <gio/gio.h>
#include <stdlib.h>
#include <string.h>

#include "nostr/nip19/nip19.h"

G_DEFINE_QUARK(nostr-dispatcher-error-quark, nd_error)

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static gboolean is_hex64(const char *s) {
  if (!s || strlen(s) != 64) return FALSE;
  for (const char *p = s; *p; p++)
    if (!g_ascii_isxdigit(*p) || g_ascii_isupper(*p)) return FALSE;
  return TRUE;
}

static gboolean hex_to_bytes32(const char *hex, guint8 out[32]) {
  if (!is_hex64(hex)) return FALSE;
  for (int i = 0; i < 32; i++)
    out[i] = (guint8)((g_ascii_xdigit_value(hex[2 * i]) << 4) |
                      g_ascii_xdigit_value(hex[2 * i + 1]));
  return TRUE;
}

static gboolean is_loopback_host(const char *host) {
  if (!host) return FALSE;
  if (g_ascii_strcasecmp(host, "localhost") == 0) return TRUE;
  /* Literal addresses only: a name like "127.example.com" is NOT loopback. */
  g_autofree char *bare = (host[0] == '[')
                              ? g_strndup(host + 1, strcspn(host + 1, "]"))
                              : g_strdup(host);
  g_autoptr(GInetAddress) a = g_inet_address_new_from_string(bare);
  return a && g_inet_address_get_is_loopback(a);
}

gboolean nd_relay_url_acceptable(const char *url) {
  if (!url || !*url || strlen(url) > 255) return FALSE;
  for (const char *p = url; *p; p++)
    if ((guchar)*p <= 0x20 || *p == 0x7f) return FALSE;
  g_autoptr(GUri) u = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
  if (!u) return FALSE;
  const char *scheme = g_uri_get_scheme(u);
  const char *host = g_uri_get_host(u);
  if (!host || !*host || g_uri_get_userinfo(u)) return FALSE;
  if (g_ascii_strcasecmp(scheme, "wss") == 0) return TRUE;
  if (g_ascii_strcasecmp(scheme, "ws") == 0) return is_loopback_host(host);
  return FALSE;
}

char **nd_relays_filter(const char *const *relays, gsize n) {
  GPtrArray *out = g_ptr_array_new();
  for (gsize i = 0; relays && i < n && out->len < ND_URI_MAX_RELAYS; i++) {
    const char *r = relays[i];
    if (!nd_relay_url_acceptable(r)) {
      g_debug("nostr-dispatcher: dropping relay hint (scheme/host not allowed)");
      continue;
    }
    gboolean dup = FALSE;
    for (guint j = 0; j < out->len; j++)
      if (strcmp(g_ptr_array_index(out, j), r) == 0) dup = TRUE;
    if (!dup) g_ptr_array_add(out, g_strdup(r));
  }
  g_ptr_array_add(out, NULL);
  return (char **)g_ptr_array_free(out, FALSE);
}

gboolean nd_bech32_tlv_kind(const char *bech, guint32 *out_kind) {
  char *hrp = NULL;
  uint8_t *tlv = NULL;
  size_t len = 0;
  gboolean found = FALSE;
  if (nostr_nip19_decode_tlv(bech, &hrp, &tlv, &len) != 0) return FALSE;
  for (size_t i = 0; i + 2 <= len;) {
    uint8_t t = tlv[i], l = tlv[i + 1];
    i += 2;
    if (i + l > len) break;
    if (t == 3 && l == 4) {
      guint32 k = ((guint32)tlv[i] << 24) | ((guint32)tlv[i + 1] << 16) |
                  ((guint32)tlv[i + 2] << 8) | (guint32)tlv[i + 3];
      if (out_kind) *out_kind = k;
      found = TRUE;
      break;
    }
    i += l;
  }
  free(hrp);
  free(tlv);
  return found;
}

void nd_target_free(NdTarget *t) {
  if (!t) return;
  g_free(t->id_hex);
  g_free(t->pubkey_hex);
  g_free(t->identifier);
  g_strfreev(t->relays);
  g_free(t);
}

NdTarget *nd_target_copy(const NdTarget *t) {
  if (!t) return NULL;
  NdTarget *c = g_new0(NdTarget, 1);
  c->entity = t->entity;
  c->id_hex = g_strdup(t->id_hex);
  c->pubkey_hex = g_strdup(t->pubkey_hex);
  c->identifier = g_strdup(t->identifier);
  c->kind = t->kind;
  c->relays = t->relays ? g_strdupv(t->relays) : g_new0(char *, 1);
  return c;
}

static NdTarget *target_new(NdEntity e) {
  NdTarget *t = g_new0(NdTarget, 1);
  t->entity = e;
  t->kind = -1;
  return t;
}

/* ------------------------------------------------------------------ */
/* Parsing                                                             */
/* ------------------------------------------------------------------ */

static NdTarget *parse_legacy_open(const char *query, GError **error) {
  g_autoptr(GHashTable) params =
      g_uri_parse_params(query, -1, "&", G_URI_PARAMS_NONE, NULL);
  const char *ev = params ? g_hash_table_lookup(params, "event") : NULL;
  if (!is_hex64(ev)) {
    g_set_error_literal(error, ND_ERROR, ND_ERROR_INVALID_URI,
                        "legacy nostr://open link without a valid event id");
    return NULL;
  }
  /* `group=` is ignored: the NIP-29 `h` tag is inside the signed event. */
  NdTarget *t = target_new(ND_ENTITY_EVENT);
  t->id_hex = g_strdup(ev);
  t->relays = g_new0(char *, 1);
  return t;
}

static NdTarget *parse_bech32(const char *bech, GError **error) {
  NostrBech32Type type = NOSTR_B32_UNKNOWN;
  if (nostr_nip19_inspect(bech, &type) != 0) type = NOSTR_B32_UNKNOWN;

  switch (type) {
  case NOSTR_B32_NOTE: {
    guint8 id[32];
    if (nostr_nip19_decode_note(bech, id) != 0) break;
    NdTarget *t = target_new(ND_ENTITY_EVENT);
    t->id_hex = g_malloc(65);
    for (int i = 0; i < 32; i++) g_snprintf(t->id_hex + 2 * i, 3, "%02x", id[i]);
    t->relays = g_new0(char *, 1);
    return t;
  }
  case NOSTR_B32_NPUB: {
    guint8 pk[32];
    if (nostr_nip19_decode_npub(bech, pk) != 0) break;
    NdTarget *t = target_new(ND_ENTITY_PROFILE);
    t->pubkey_hex = g_malloc(65);
    for (int i = 0; i < 32; i++) g_snprintf(t->pubkey_hex + 2 * i, 3, "%02x", pk[i]);
    t->kind = 0;
    t->relays = g_new0(char *, 1);
    return t;
  }
  case NOSTR_B32_NPROFILE: {
    NostrProfilePointer *p = NULL;
    if (nostr_nip19_decode_nprofile(bech, &p) != 0 || !p) break;
    NdTarget *t = NULL;
    if (is_hex64(p->public_key)) {
      t = target_new(ND_ENTITY_PROFILE);
      t->pubkey_hex = g_strdup(p->public_key);
      t->kind = 0;
      t->relays = nd_relays_filter((const char *const *)p->relays, p->relays_count);
    }
    nostr_profile_pointer_free(p);
    if (t) return t;
    break;
  }
  case NOSTR_B32_NEVENT: {
    NostrEventPointer *e = NULL;
    if (nostr_nip19_decode_nevent(bech, &e) != 0 || !e) break;
    NdTarget *t = NULL;
    if (is_hex64(e->id)) {
      t = target_new(ND_ENTITY_EVENT);
      t->id_hex = g_strdup(e->id);
      if (is_hex64(e->author)) t->pubkey_hex = g_strdup(e->author);
      t->relays = nd_relays_filter((const char *const *)e->relays, e->relays_count);
      /* NostrEventPointer.kind conflates "absent" with kind 0; walk the
       * TLV ourselves so a real kind-0 TLV is honoured. */
      guint32 k = 0;
      if (nd_bech32_tlv_kind(bech, &k) && k <= 65535) t->kind = (gint)k;
    }
    nostr_event_pointer_free(e);
    if (t) return t;
    break;
  }
  case NOSTR_B32_NADDR: {
    NostrEntityPointer *a = NULL;
    if (nostr_nip19_decode_naddr(bech, &a) != 0 || !a) break;
    NdTarget *t = NULL;
    if (is_hex64(a->public_key) && a->kind >= 0 && a->kind <= 65535) {
      t = target_new(ND_ENTITY_ADDRESS);
      t->pubkey_hex = g_strdup(a->public_key);
      t->identifier = g_strdup(a->identifier ? a->identifier : "");
      t->kind = a->kind;
      t->relays = nd_relays_filter((const char *const *)a->relays, a->relays_count);
    }
    nostr_entity_pointer_free(a);
    if (t) return t;
    break;
  }
  case NOSTR_B32_NSEC:
    g_set_error_literal(error, ND_ERROR, ND_ERROR_FORBIDDEN,
                        "refusing to handle a secret key (nsec) link");
    return NULL;
  default:
    break;
  }
  g_set_error_literal(error, ND_ERROR, ND_ERROR_INVALID_URI,
                      "not a supported NIP-19 entity (expected note, nevent, "
                      "naddr, npub or nprofile)");
  return NULL;
}

NdTarget *nd_target_parse_uri(const char *uri, GError **error) {
  if (!uri || !*uri || strlen(uri) > ND_URI_MAX_LEN) {
    g_set_error_literal(error, ND_ERROR, ND_ERROR_INVALID_URI,
                        "empty or oversized URI");
    return NULL;
  }

  /* Secret-key guard runs FIRST, before any code path that might echo the
   * input into a log or error message. */
  g_autofree char *lower = g_ascii_strdown(uri, -1);
  if (strstr(lower, "nsec1") || strstr(lower, "ncryptsec1")) {
    g_set_error_literal(error, ND_ERROR, ND_ERROR_FORBIDDEN,
                        "refusing to handle a secret key (nsec) link");
    return NULL;
  }

  const char *rest = lower;
  if (g_str_has_prefix(rest, "web+nostr:"))
    rest += strlen("web+nostr:");
  else if (g_str_has_prefix(rest, "nostr:"))
    rest += strlen("nostr:");

  if (g_str_has_prefix(rest, "//")) {
    rest += 2;
    if (g_str_has_prefix(rest, "open?")) {
      /* Legacy notify form. Parse from the original (case-preserved)
       * string so percent-escapes survive; ids must be lowercase anyway. */
      const char *q = strchr(uri, '?');
      return parse_legacy_open(q ? q + 1 : "", error);
    }
  }

  g_autofree char *bech = g_strndup(rest, strcspn(rest, "?#/"));
  if (!*bech) {
    g_set_error_literal(error, ND_ERROR, ND_ERROR_INVALID_URI,
                        "URI carries no NIP-19 identifier");
    return NULL;
  }
  return parse_bech32(bech, error);
}

/* ------------------------------------------------------------------ */
/* Encoding                                                            */
/* ------------------------------------------------------------------ */

static gboolean tlv_append(GByteArray *b, guint8 type, const guint8 *v, gsize l) {
  if (l > 255) return FALSE;
  guint8 hdr[2] = {type, (guint8)l};
  g_byte_array_append(b, hdr, 2);
  g_byte_array_append(b, v, (guint)l);
  return TRUE;
}

static char *encode_nevent(const NdTarget *t) {
  guint8 id[32], au[32];
  if (!hex_to_bytes32(t->id_hex, id)) return NULL;
  g_autoptr(GByteArray) b = g_byte_array_new();
  tlv_append(b, 0, id, 32);
  for (char **r = t->relays; r && *r; r++)
    if (!tlv_append(b, 1, (const guint8 *)*r, strlen(*r))) return NULL;
  if (t->pubkey_hex && hex_to_bytes32(t->pubkey_hex, au)) tlv_append(b, 2, au, 32);
  if (t->kind >= 0) {
    guint32 k = (guint32)t->kind;
    guint8 kb[4] = {(guint8)(k >> 24), (guint8)(k >> 16), (guint8)(k >> 8), (guint8)k};
    tlv_append(b, 3, kb, 4);
  }
  char *out = NULL;
  if (nostr_nip19_encode_tlv("nevent", b->data, b->len, &out) != 0) return NULL;
  return out;
}

char *nd_target_to_uri(const NdTarget *t) {
  if (!t) return NULL;
  char *bech = NULL;
  gsize nrelays = t->relays ? g_strv_length(t->relays) : 0;

  switch (t->entity) {
  case ND_ENTITY_EVENT: {
    guint8 id[32];
    if (nrelays == 0 && t->kind < 0 && !t->pubkey_hex) {
      if (!hex_to_bytes32(t->id_hex, id) || nostr_nip19_encode_note(id, &bech) != 0)
        return NULL;
    } else {
      bech = encode_nevent(t);
    }
    break;
  }
  case ND_ENTITY_ADDRESS: {
    NostrEntityPointer a = {
        .public_key = t->pubkey_hex,
        .kind = t->kind,
        .identifier = t->identifier ? t->identifier : (char *)"",
        .relays = t->relays,
        .relays_count = nrelays,
    };
    if (nostr_nip19_encode_naddr(&a, &bech) != 0) return NULL;
    break;
  }
  case ND_ENTITY_PROFILE: {
    if (nrelays == 0) {
      guint8 pk[32];
      if (!hex_to_bytes32(t->pubkey_hex, pk) || nostr_nip19_encode_npub(pk, &bech) != 0)
        return NULL;
    } else {
      NostrProfilePointer p = {
          .public_key = t->pubkey_hex, .relays = t->relays, .relays_count = nrelays};
      if (nostr_nip19_encode_nprofile(&p, &bech) != 0) return NULL;
    }
    break;
  }
  default:
    return NULL;
  }
  if (!bech) return NULL;
  char *uri = g_strconcat("nostr:", bech, NULL);
  free(bech);
  return uri;
}
