/*
 * session_fed_policy.c — see session_fed_policy.h. No I/O except reading
 * session-relay.conf in nsr_fed_config_load().
 */
#include "session_fed_policy.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nostr-publish/nostr-publish-policy.h>

#include "json.h"
#include "nostr-kinds.h"
#include "session_routing.h"

/* ── Config ───────────────────────────────────────────────────────────── */

void nsr_fed_config_defaults(NsrFedConfig *cfg) {
  memset(cfg, 0, sizeof *cfg);
  cfg->enabled = 1;
  cfg->backoff_initial_seconds = 15;
  cfg->backoff_max_seconds = 3600;
  cfg->ok_timeout_seconds = 30;
  cfg->max_age_seconds = 7 * 24 * 3600;
  cfg->keep_settled_seconds = 7 * 24 * 3600;
  cfg->max_relays_per_event = 16;
  cfg->max_inflight_per_relay = 32;
  cfg->idle_disconnect_seconds = 60;
}

static char *trim(char *s) {
  while (*s && isspace((unsigned char)*s)) s++;
  size_t n = strlen(s);
  while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
  return s;
}

static int parse_int_range(const char *val, int lo, int hi, int *out) {
  char *end = NULL;
  errno = 0;
  long v = strtol(val, &end, 10);
  if (errno || !end || end == val || *trim(end) != '\0') return -1;
  if (v < lo || v > hi) return -1;
  *out = (int)v;
  return 0;
}

static int parse_u32(const char *s, uint32_t *out) {
  char *end = NULL;
  errno = 0;
  unsigned long v = strtoul(s, &end, 10);
  if (errno || end == s || v > UINT32_MAX) return -1;
  if (*trim(end) != '\0') return -1;
  *out = (uint32_t)v;
  return 0;
}

static int parse_kind_ranges(NsrFedConfig *cfg, const char *val) {
  cfg->n_local_only = 0;
  char *copy = g_strdup(val);
  int rc = 0;
  char *save = NULL;
  for (char *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
    char *t = trim(tok);
    if (!*t) continue;
    if (cfg->n_local_only >= NSR_FED_MAX_KIND_RANGES) { rc = -1; break; }
    NsrFedKindRange r;
    char *dash = strchr(t, '-');
    if (dash) {
      *dash = '\0';
      if (parse_u32(trim(t), &r.lo) || parse_u32(trim(dash + 1), &r.hi) || r.lo > r.hi) {
        rc = -1;
        break;
      }
    } else {
      if (parse_u32(t, &r.lo)) { rc = -1; break; }
      r.hi = r.lo;
    }
    cfg->local_only[cfg->n_local_only++] = r;
  }
  g_free(copy);
  return rc;
}

static int parse_accounts(NsrFedConfig *cfg, const char *val) {
  cfg->n_accounts = 0;
  char *copy = g_strdup(val);
  int rc = 0;
  char *save = NULL;
  for (char *tok = strtok_r(copy, ", \t", &save); tok; tok = strtok_r(NULL, ", \t", &save)) {
    if (cfg->n_accounts >= NSR_FED_MAX_ACCOUNTS) { rc = -1; break; }
    char hex[65];
    if (nsr_fed_parse_pubkey(tok, hex) != 0) { rc = -1; break; }
    if (nsr_fed_config_has_account(cfg, hex)) continue;
    memcpy(cfg->accounts[cfg->n_accounts++], hex, sizeof hex);
  }
  g_free(copy);
  return rc;
}

int nsr_fed_config_apply(NsrFedConfig *cfg, const char *key, const char *val) {
#define INT_KEY(name, field, lo, hi)                                      \
  if (strcmp(key, name) == 0)                                             \
    return parse_int_range(val, lo, hi, &cfg->field) == 0 ? 1 : -1;
  INT_KEY("federation", enabled, 0, 1)
  INT_KEY("federation_allow_plaintext_ws", allow_plaintext_ws, 0, 1)
  INT_KEY("federation_backoff_initial_seconds", backoff_initial_seconds, 1, 86400)
  INT_KEY("federation_backoff_max_seconds", backoff_max_seconds, 1, 7 * 86400)
  INT_KEY("federation_ok_timeout_seconds", ok_timeout_seconds, 1, 3600)
  INT_KEY("federation_max_age_seconds", max_age_seconds, 60, 365 * 86400)
  INT_KEY("federation_keep_settled_seconds", keep_settled_seconds, 0, 365 * 86400)
  INT_KEY("federation_max_relays_per_event", max_relays_per_event, 1, 64)
  INT_KEY("federation_max_inflight_per_relay", max_inflight_per_relay, 1, 1024)
  INT_KEY("federation_idle_disconnect_seconds", idle_disconnect_seconds, 1, 86400)
#undef INT_KEY
  if (strcmp(key, "federation_accounts") == 0)
    return parse_accounts(cfg, val) == 0 ? 1 : -1;
  if (strcmp(key, "federation_local_only_kinds") == 0)
    return parse_kind_ranges(cfg, val) == 0 ? 1 : -1;
  if (strncmp(key, "federation", 10) == 0) return -1; /* typo'd federation key */
  return 0;
}

int nsr_fed_config_load(const char *path, NsrFedConfig *cfg, char *err, size_t err_sz) {
  nsr_fed_config_defaults(cfg);
  if (err && err_sz) err[0] = '\0';
  if (!path) return 0;
  FILE *f = fopen(path, "r");
  if (!f) return 0;
  char line[2048];
  int rc = 0, lineno = 0;
  while (fgets(line, sizeof line, f)) {
    lineno++;
    char *l = trim(line);
    if (!*l || *l == '#' || *l == ';') continue;
    char *eq = strchr(l, '=');
    if (!eq) continue; /* relayd_config_load() owns grammar errors */
    *eq = '\0';
    char *key = trim(l), *val = trim(eq + 1);
    size_t vl = strlen(val);
    if (vl >= 2 && val[0] == '"' && val[vl - 1] == '"') {
      val[vl - 1] = '\0';
      val++;
    }
    if (nsr_fed_config_apply(cfg, key, val) < 0) {
      if (err && err_sz)
        snprintf(err, err_sz, "%s:%d: bad value for %s", path, lineno, key);
      rc = -1;
      break;
    }
  }
  fclose(f);
  if (rc == 0 && cfg->backoff_max_seconds < cfg->backoff_initial_seconds)
    cfg->backoff_max_seconds = cfg->backoff_initial_seconds;
  return rc;
}

gboolean nsr_fed_config_has_account(const NsrFedConfig *cfg, const char *pubkey_hex) {
  if (!pubkey_hex) return FALSE;
  for (size_t i = 0; i < cfg->n_accounts; i++)
    if (g_ascii_strcasecmp(cfg->accounts[i], pubkey_hex) == 0) return TRUE;
  return FALSE;
}

/* ── NIP-19 npub (bech32) ─────────────────────────────────────────────── */

static uint32_t bech32_polymod(const uint8_t *v, size_t n) {
  static const uint32_t G[5] = {0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3};
  uint32_t chk = 1;
  for (size_t i = 0; i < n; i++) {
    uint8_t top = (uint8_t)(chk >> 25);
    chk = ((chk & 0x1ffffff) << 5) ^ v[i];
    for (int j = 0; j < 5; j++)
      if ((top >> j) & 1) chk ^= G[j];
  }
  return chk;
}

static int decode_npub(const char *s, uint8_t out[32]) {
  static const char CHARSET[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
  size_t len = strlen(s);
  if (len != 63 || g_ascii_strncasecmp(s, "npub1", 5) != 0) return -1;
  /* hrp expansion ("npub") + data part */
  uint8_t v[4 * 2 + 1 + 58];
  size_t n = 0;
  const char *hrp = "npub";
  for (int i = 0; i < 4; i++) v[n++] = (uint8_t)(hrp[i] >> 5);
  v[n++] = 0;
  for (int i = 0; i < 4; i++) v[n++] = (uint8_t)(hrp[i] & 31);
  uint8_t data[58];
  for (size_t i = 5; i < len; i++) {
    const char *p = strchr(CHARSET, g_ascii_tolower(s[i]));
    if (!p || !*p) return -1;
    data[i - 5] = (uint8_t)(p - CHARSET);
    v[n++] = data[i - 5];
  }
  if (bech32_polymod(v, n) != 1) return -1;
  /* 52 data groups of 5 bits (6 checksum groups excluded) → 32 bytes. */
  uint32_t acc = 0;
  int bits = 0;
  size_t o = 0;
  for (size_t i = 0; i < 52; i++) {
    acc = (acc << 5) | data[i];
    bits += 5;
    while (bits >= 8) {
      bits -= 8;
      if (o >= 32) return -1;
      out[o++] = (uint8_t)((acc >> bits) & 0xff);
    }
  }
  if (o != 32 || bits >= 5 || (acc & ((1u << bits) - 1)) != 0) return -1;
  return 0;
}

int nsr_fed_parse_pubkey(const char *text, char out_hex[65]) {
  if (!text) return -1;
  size_t n = strlen(text);
  if (n == 64) {
    for (size_t i = 0; i < 64; i++) {
      if (!g_ascii_isxdigit(text[i])) return -1;
      out_hex[i] = g_ascii_tolower(text[i]);
    }
    out_hex[64] = '\0';
    return 0;
  }
  uint8_t raw[32];
  if (decode_npub(text, raw) != 0) return -1;
  static const char HX[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    out_hex[2 * i] = HX[raw[i] >> 4];
    out_hex[2 * i + 1] = HX[raw[i] & 15];
  }
  out_hex[64] = '\0';
  return 0;
}

/* ── Local-only contract ──────────────────────────────────────────────── */

gboolean nsr_fed_kind_is_ephemeral(int kind) { return kind >= 20000 && kind < 30000; }
gboolean nsr_fed_kind_is_replaceable(int kind) {
  return kind == 0 || kind == 3 || (kind >= 10000 && kind < 20000);
}
gboolean nsr_fed_kind_is_addressable(int kind) { return kind >= 30000 && kind < 40000; }

static gboolean has_protected_tag(NostrTags *tags) {
  size_t n = tags ? nostr_tags_size(tags) : 0;
  for (size_t i = 0; i < n; i++) {
    NostrTag *t = nostr_tags_get(tags, i);
    const char *k = t ? nostr_tag_get_key(t) : NULL;
    if (k && strcmp(k, "-") == 0) return TRUE;
  }
  return FALSE;
}

NsrFedVerdict nsr_fed_static_verdict(const NsrFedConfig *cfg, int kind, NostrTags *tags) {
  if (kind == 13 || kind == 14 || kind == 22242 || nsr_fed_kind_is_ephemeral(kind))
    return NSR_FED_SKIP_NEVER_KIND;
  if (has_protected_tag(tags)) return NSR_FED_SKIP_PROTECTED;
  for (size_t i = 0; cfg && i < cfg->n_local_only; i++)
    if ((uint32_t)kind >= cfg->local_only[i].lo && (uint32_t)kind <= cfg->local_only[i].hi)
      return NSR_FED_SKIP_LOCAL_KIND;
  return NSR_FED_FORWARD;
}

const char *nsr_fed_verdict_reason(NsrFedVerdict v) {
  switch (v) {
    case NSR_FED_FORWARD: return "";
    case NSR_FED_SKIP_PROTECTED: return "local-only: NIP-70 protected event ([\"-\"] tag)";
    case NSR_FED_SKIP_NEVER_KIND: return "local-only: kind is never forwarded (seal/rumor/auth/ephemeral)";
    case NSR_FED_SKIP_LOCAL_KIND: return "local-only: kind listed in federation_local_only_kinds";
  }
  return "local-only";
}

static const char *first_tag_value(NostrTags *tags, const char *key) {
  size_t n = tags ? nostr_tags_size(tags) : 0;
  for (size_t i = 0; i < n; i++) {
    NostrTag *t = nostr_tags_get(tags, i);
    if (!t || nostr_tag_size(t) < 2) continue;
    const char *k = nostr_tag_get_key(t);
    if (k && strcmp(k, key) == 0) return nostr_tag_get(t, 1);
  }
  return NULL;
}

char *nsr_fed_replace_key(int kind, const char *pubkey, NostrTags *tags) {
  if (!pubkey) return NULL;
  if (nsr_fed_kind_is_replaceable(kind)) return g_strdup_printf("%d:%s:", kind, pubkey);
  if (nsr_fed_kind_is_addressable(kind)) {
    const char *d = first_tag_value(tags, "d");
    return g_strdup_printf("%d:%s:%s", kind, pubkey, d ? d : "");
  }
  return NULL;
}

/* ── URLs ─────────────────────────────────────────────────────────────── */

static gboolean host_is_loopback(const char *host, size_t len) {
  if (len == 9 && g_ascii_strncasecmp(host, "localhost", 9) == 0) return TRUE;
  if (len == 5 && memcmp(host, "[::1]", 5) == 0) return TRUE;
  if (len >= 4 && memcmp(host, "127.", 4) == 0) {
    for (size_t i = 4; i < len; i++)
      if (!g_ascii_isdigit(host[i]) && host[i] != '.') return FALSE;
    return TRUE;
  }
  return FALSE;
}

gboolean nsr_fed_url_acceptable(const char *url, gboolean allow_plaintext_ws) {
  if (!url) return FALSE;
  size_t n = strlen(url);
  if (n == 0 || n > NSR_FED_MAX_URL_LEN) return FALSE;
  for (size_t i = 0; i < n; i++)
    if ((unsigned char)url[i] <= 0x20 || url[i] == 0x7f) return FALSE;
  gboolean tls;
  const char *rest;
  if (g_ascii_strncasecmp(url, "wss://", 6) == 0) {
    tls = TRUE;
    rest = url + 6;
  } else if (g_ascii_strncasecmp(url, "ws://", 5) == 0) {
    tls = FALSE;
    rest = url + 5;
  } else {
    return FALSE;
  }
  size_t auth_len = strcspn(rest, "/?#");
  if (auth_len == 0) return FALSE;
  if (memchr(rest, '@', auth_len)) return FALSE; /* no userinfo */
  /* host = authority minus :port (bracketed IPv6 kept whole) */
  size_t host_len = auth_len;
  if (rest[0] == '[') {
    const char *rb = memchr(rest, ']', auth_len);
    if (!rb) return FALSE;
    host_len = (size_t)(rb - rest) + 1;
  } else {
    const char *colon = memchr(rest, ':', auth_len);
    if (colon) host_len = (size_t)(colon - rest);
  }
  if (host_len == 0) return FALSE;
  if (!tls && !allow_plaintext_ws && !host_is_loopback(rest, host_len)) return FALSE;
  return TRUE;
}

/* ── Relay lists ──────────────────────────────────────────────────────── */

static void add_url(GPtrArray *out, const NsrFedConfig *cfg, const char *url) {
  if (!nsr_fed_url_acceptable(url, cfg->allow_plaintext_ws)) return;
  for (guint i = 0; i < out->len; i++)
    if (strcmp(g_ptr_array_index(out, i), url) == 0) return;
  g_ptr_array_add(out, g_strdup(url));
}

static GStrv finish_strv(GPtrArray *a) {
  g_ptr_array_add(a, NULL);
  return (GStrv)g_ptr_array_free(a, FALSE);
}

GStrv nsr_fed_write_relays_from_10002(const NsrFedConfig *cfg, const char *json) {
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  if (json) {
    GStrv w = nostr_publish_nip65_relays(json, NOSTR_PUBLISH_NIP65_WRITE, NULL);
    for (guint i = 0; w && w[i]; i++) add_url(out, cfg, w[i]);
    g_strfreev(w);
  }
  g_ptr_array_set_free_func(out, NULL);
  return finish_strv(out);
}

static NostrEvent *parse_event(const char *json) {
  if (!json) return NULL;
  NostrEvent *ev = nostr_event_new();
  if (ev && nostr_event_deserialize(ev, json) != 0) {
    nostr_event_free(ev);
    return NULL;
  }
  return ev;
}

GStrv nsr_fed_inbox_relays_from_10050(const NsrFedConfig *cfg, const char *json) {
  GPtrArray *out = g_ptr_array_new();
  NostrEvent *ev = parse_event(json);
  if (ev && nostr_event_get_kind(ev) == 10050) {
    NostrTags *tags = nostr_event_get_tags(ev);
    size_t n = tags ? nostr_tags_size(tags) : 0;
    for (size_t i = 0; i < n; i++) {
      NostrTag *t = nostr_tags_get(tags, i);
      if (!t || nostr_tag_size(t) < 2) continue;
      const char *k = nostr_tag_get_key(t);
      if (k && strcmp(k, "relay") == 0) add_url(out, cfg, nostr_tag_get(t, 1));
    }
  }
  if (ev) nostr_event_free(ev);
  return finish_strv(out);
}

/* Comparison key for "is this the same relay?": scheme and authority
 * lowercased, a bare trailing "/" dropped. Only used to tell entries
 * apart; the listed URL is what gets used. */
static char *relay_key(const char *url) {
  const char *sep = strstr(url, "://");
  const char *auth = sep ? sep + 3 : url;
  size_t auth_len = strcspn(auth, "/?#");
  GString *k = g_string_new(NULL);
  for (const char *c = url; c < auth + auth_len; c++) g_string_append_c(k, g_ascii_tolower(*c));
  const char *rest = auth + auth_len;
  if (strcmp(rest, "/") != 0) g_string_append(k, rest);
  return g_string_free(k, FALSE);
}

GStrv nsr_fed_group_relays_from_10009(const char *json, const char *group_id) {
  GPtrArray *out = g_ptr_array_new();
  GPtrArray *keys = g_ptr_array_new_with_free_func(g_free);
  NostrEvent *ev = group_id && *group_id ? parse_event(json) : NULL;
  if (ev && nostr_event_get_kind(ev) == NOSTR_KIND_SIMPLE_GROUP_LIST) {
    NostrTags *tags = nostr_event_get_tags(ev);
    size_t n = tags ? nostr_tags_size(tags) : 0;
    for (size_t i = 0; i < n; i++) {
      NostrTag *t = nostr_tags_get(tags, i);
      if (!t || nostr_tag_size(t) < 3) continue;
      const char *k = nostr_tag_get_key(t);
      if (!k || strcmp(k, "group") != 0) continue;
      if (g_strcmp0(nostr_tag_get(t, 1), group_id) != 0) continue;
      const char *url = nostr_tag_get(t, 2);
      if (!url || !*url) continue;
      char *key = relay_key(url);
      gboolean seen = FALSE;
      for (guint j = 0; j < keys->len && !seen; j++) seen = !strcmp(keys->pdata[j], key);
      if (seen) {
        g_free(key);
        continue;
      }
      g_ptr_array_add(keys, key);
      g_ptr_array_add(out, g_strdup(url));
    }
  }
  if (ev) nostr_event_free(ev);
  g_ptr_array_unref(keys);
  return finish_strv(out);
}

/* ── Route resolution ─────────────────────────────────────────────────── */

static void add_strv(GPtrArray *out, const NsrFedConfig *cfg, GStrv v) {
  for (guint i = 0; v && v[i]; i++) add_url(out, cfg, v[i]);
}

/* First tag @key with a non-empty value, or NULL. */
static NostrTag *first_tag_nonempty(NostrTags *tags, const char *key) {
  size_t n = tags ? nostr_tags_size(tags) : 0;
  for (size_t i = 0; i < n; i++) {
    NostrTag *t = nostr_tags_get(tags, i);
    const char *k = t ? nostr_tag_get_key(t) : NULL;
    if (!k || strcmp(k, key) != 0 || nostr_tag_size(t) < 2) continue;
    const char *v = nostr_tag_get(t, 1);
    if (v && *v) return t;
  }
  return NULL;
}

static gboolean kind_is_group_metadata(int kind) {
  return kind >= NOSTR_KIND_SIMPLE_GROUP_METADATA && kind <= NOSTR_KIND_SIMPLE_GROUP_PINNED_EVENTS;
}

/* The tag naming the group an event addresses: `h` (events sent to a
 * group), or `d` for the relay-generated 39000-39005 metadata. */
static NostrTag *group_id_tag(NostrEvent *ev) {
  NostrTags *tags = nostr_event_get_tags(ev);
  return first_tag_nonempty(tags, kind_is_group_metadata(nostr_event_get_kind(ev)) ? "d" : "h");
}

/* NIP-29 @ db5fe3d: a group is (relay, id). Forks keep the id and live on
 * other relays with their own members, admins and history, so the relay is
 * never guessed: it is the one the event names, or the only one the
 * author's kind-10009 lists for that id. Several entries for the same id
 * (the user is in several forks) are ambiguous: the event waits until it
 * names one, instead of silently going to the fork listed first. There is
 * no fallback to another relay for the same id, and none to home relays;
 * an unreachable group relay keeps the write queued (retried until
 * federation_max_age_seconds) — finding where a group moved is a client
 * decision (NIP-29: consult the admins' kind 10009), not a relay reroute. */
static NsrFedRouteStatus resolve_group(const NsrFedConfig *cfg, NostrEvent *ev,
                                       const NsrFedLookup *lk, GPtrArray *out,
                                       char **reason) {
  NostrTag *g = group_id_tag(ev);
  if (!g) {
    *reason = g_strdup(kind_is_group_metadata(nostr_event_get_kind(ev))
                           ? "invalid: NIP-29 group metadata without a group id (d tag)"
                           : "invalid: NIP-29 group event without a group id (h tag)");
    return NSR_FED_ROUTE_INVALID;
  }
  const char *gid = nostr_tag_get(g, 1);
  /* 1. The relay named by the event itself: ["h", id, relay]. Not a NIP-29
   * shape (the h tag carries the bare id) but a nostrc extension that lets
   * a client pick the fork; relays ignore extra tag elements. */
  const char *named = strcmp(nostr_tag_get_key(g), "h") == 0 && nostr_tag_size(g) >= 3
                          ? nostr_tag_get(g, 2) : NULL;
  if (named && *named) {
    if (!nsr_fed_url_acceptable(named, cfg->allow_plaintext_ws)) {
      *reason = g_strdup_printf("group '%s': the relay named in the h tag (%s) is not an "
                                "admissible relay URL", gid, named);
      return NSR_FED_ROUTE_UNROUTABLE;
    }
    add_url(out, cfg, named);
    return NSR_FED_ROUTE_OK;
  }
  /* 2. The author's kind-10009 simple-groups list. */
  GStrv listed = NULL;
  if (lk && lk->relay_list_json) {
    char *json = lk->relay_list_json(lk->ud, nostr_event_get_pubkey(ev),
                                     NOSTR_KIND_SIMPLE_GROUP_LIST);
    listed = nsr_fed_group_relays_from_10009(json, gid);
    g_free(json);
  }
  guint n = listed ? g_strv_length(listed) : 0;
  NsrFedRouteStatus st = NSR_FED_ROUTE_UNROUTABLE;
  if (n == 1 && nsr_fed_url_acceptable(listed[0], cfg->allow_plaintext_ws)) {
    add_url(out, cfg, listed[0]);
    st = NSR_FED_ROUTE_OK;
  } else if (n == 1) {
    *reason = g_strdup_printf("group '%s': your kind-10009 lists it on %s, which is not an "
                              "admissible relay URL", gid, listed[0]);
  } else if (n > 1) {
    char *all = g_strjoinv(", ", listed);
    *reason = g_strdup_printf(
        "group '%s' is listed on %u relays in your kind-10009 (%s): a NIP-29 group is "
        "(relay, id) and forks share the id, so the event must name its relay "
        "([\"h\", id, relay])", gid, n, all);
    g_free(all);
  } else {
    /* The h tag carries the bare id; "host'id" is a client-side reference
     * form that a group relay would reject, so no relay is derived from it. */
    *reason = g_strdup_printf(
        "no relay known for group '%s': add it to your kind-10009 list%s", gid,
        strchr(gid, '\'') ? " (the h tag must carry the bare group id, not host'id)" : "");
  }
  g_strfreev(listed);
  return st;
}

static NsrFedRouteStatus resolve_home(const NsrFedConfig *cfg, NostrEvent *ev,
                                      const NsrFedLookup *lk, GPtrArray *out,
                                      char **reason) {
  const char *pk = nostr_event_get_pubkey(ev);
  int kind = nostr_event_get_kind(ev);
  if (kind == 10002) {
    /* A relay-list update goes to the relays it names (write set). */
    char *self = nostr_event_serialize(ev);
    GStrv w = nsr_fed_write_relays_from_10002(cfg, self);
    add_strv(out, cfg, w);
    g_strfreev(w);
    free(self);
  }
  if (lk && lk->relay_list_json) {
    char *json = lk->relay_list_json(lk->ud, pk, 10002);
    GStrv w = nsr_fed_write_relays_from_10002(cfg, json);
    add_strv(out, cfg, w);
    g_strfreev(w);
    g_free(json);
  }
  if (kind == 5 && lk && lk->acked_relays) {
    /* NIP-09: also where the deleted events actually went. */
    NostrTags *tags = nostr_event_get_tags(ev);
    size_t n = tags ? nostr_tags_size(tags) : 0;
    for (size_t i = 0; i < n; i++) {
      NostrTag *t = nostr_tags_get(tags, i);
      const char *k = t ? nostr_tag_get_key(t) : NULL;
      if (!k || nostr_tag_size(t) < 2) continue;
      gboolean coord = strcmp(k, "a") == 0;
      if (!coord && strcmp(k, "e") != 0) continue;
      GStrv r = lk->acked_relays(lk->ud, nostr_tag_get(t, 1), coord);
      add_strv(out, cfg, r);
      g_strfreev(r);
    }
  }
  if (out->len == 0) {
    *reason = g_strdup("no NIP-65 write relays: the author's kind-10002 relay "
                       "list is not in the session relay");
    return NSR_FED_ROUTE_UNROUTABLE;
  }
  return NSR_FED_ROUTE_OK;
}

static NsrFedRouteStatus resolve_inbox(const NsrFedConfig *cfg, NostrEvent *ev,
                                       const NsrFedLookup *lk, GPtrArray *out,
                                       char **reason) {
  const char *p = first_tag_value(nostr_event_get_tags(ev), "p");
  char hex[65];
  if (!p || nsr_fed_parse_pubkey(p, hex) != 0) {
    *reason = g_strdup("invalid: gift wrap without a recipient p tag");
    return NSR_FED_ROUTE_INVALID;
  }
  if (lk && lk->relay_list_json) {
    char *json = lk->relay_list_json(lk->ud, hex, 10050);
    GStrv r = nsr_fed_inbox_relays_from_10050(cfg, json);
    add_strv(out, cfg, r);
    g_strfreev(r);
    g_free(json);
  }
  if (out->len == 0) {
    /* NIP-17: no kind 10050 means the recipient is not ready for NIP-17
     * DMs; there is deliberately no fallback to their 10002 relays. */
    *reason = g_strdup_printf("no NIP-17 inbox relays: recipient %.16s…'s "
                              "kind-10050 list is not in the session relay", hex);
    return NSR_FED_ROUTE_UNROUTABLE;
  }
  return NSR_FED_ROUTE_OK;
}

static void set_basis(NsrFedBasis *b, const char *pubkey, int kind) {
  if (!b) return;
  char hex[65];
  if (!pubkey || nsr_fed_parse_pubkey(pubkey, hex) != 0) return;
  memcpy(b->pubkey, hex, sizeof hex);
  b->kind = kind;
}

NsrFedRouteStatus nsr_fed_resolve(const NsrFedConfig *cfg, NostrEvent *ev,
                                  const NsrFedLookup *lookup, GStrv *out_relays,
                                  NsrFedLane *out_lane, char **out_reason,
                                  NsrFedBasis *out_basis) {
  if (out_basis) memset(out_basis, 0, sizeof *out_basis);
  *out_relays = NULL;
  *out_reason = NULL;
  *out_lane = NSR_FED_LANE_IDENTIFIED;
  int kind = nostr_event_get_kind(ev);
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  NsrFedRouteStatus st;
  gboolean has_h = first_tag_nonempty(nostr_event_get_tags(ev), "h") != NULL;
  switch (nostr_session_route_class_event((uint32_t)kind, has_h)) {
    case NSR_ROUTE_GROUP_RELAY:
      st = resolve_group(cfg, ev, lookup, out, out_reason);
      /* A routed group write keeps its relay (identity); one still waiting
       * follows the author's kind-10009 list. */
      if (st == NSR_FED_ROUTE_UNROUTABLE)
        set_basis(out_basis, nostr_event_get_pubkey(ev), NOSTR_KIND_SIMPLE_GROUP_LIST);
      break;
    case NSR_ROUTE_NIP17_INBOX:
      if (kind != 1059) {
        st = NSR_FED_ROUTE_INVALID;
        *out_reason = g_strdup(nsr_fed_verdict_reason(NSR_FED_SKIP_NEVER_KIND));
        break;
      }
      *out_lane = NSR_FED_LANE_ANONYMOUS;
      st = resolve_inbox(cfg, ev, lookup, out, out_reason);
      if (st != NSR_FED_ROUTE_INVALID)
        set_basis(out_basis, first_tag_value(nostr_event_get_tags(ev), "p"),
                  NOSTR_KIND_DM_RELAY_LIST);
      break;
    case NSR_ROUTE_TOMBSTONE:
    case NSR_ROUTE_HOME_RELAYS:
    default:
      st = resolve_home(cfg, ev, lookup, out, out_reason);
      set_basis(out_basis, nostr_event_get_pubkey(ev), NOSTR_KIND_RELAY_LIST_METADATA);
      break;
  }
  if (st == NSR_FED_ROUTE_OK) {
    while (cfg->max_relays_per_event > 0 && out->len > (guint)cfg->max_relays_per_event)
      g_ptr_array_remove_index(out, out->len - 1);
    g_ptr_array_set_free_func(out, NULL);
    *out_relays = finish_strv(out);
  } else {
    g_ptr_array_unref(out);
  }
  return st;
}

/* ── Retry ────────────────────────────────────────────────────────────── */

int64_t nsr_fed_backoff_delay(const NsrFedConfig *cfg, unsigned attempts, double jitter01) {
  int64_t initial = cfg->backoff_initial_seconds > 0 ? cfg->backoff_initial_seconds : 15;
  int64_t max = cfg->backoff_max_seconds > 0 ? cfg->backoff_max_seconds : 3600;
  if (max < initial) max = initial;
  unsigned shift = attempts > 0 ? attempts - 1 : 0;
  int64_t base = initial;
  while (shift-- > 0 && base < max) base *= 2;
  if (base > max) base = max;
  if (jitter01 < 0) jitter01 = 0;
  if (jitter01 >= 1) jitter01 = 0.999999;
  int64_t d = (int64_t)((double)base * (0.8 + 0.4 * jitter01));
  if (d > max) d = max;
  if (d < 1) d = 1;
  return d;
}

NsrFedOkClass nsr_fed_classify_ok(gboolean accepted, const char *reason) {
  if (!accepted && reason && g_str_has_prefix(reason, "auth-required:"))
    return NSR_FED_OK_AUTH_REQUIRED;
  switch (nostr_publish_classify_ok(accepted, reason)) {
    case NOSTR_PUBLISH_OK_ACCEPT: return NSR_FED_OK_ACCEPTED;
    case NOSTR_PUBLISH_OK_PERMANENT: return NSR_FED_OK_PERMANENT;
    case NOSTR_PUBLISH_OK_TRANSIENT:
    default: return NSR_FED_OK_TRANSIENT;
  }
}
