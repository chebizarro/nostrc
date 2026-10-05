/* nm_webln.c - WebLN <-> org.nostr.Wallet1 translation (nostrc-jjyp) */
#include "nm_webln.h"

#include <string.h>

NmSatsStatus nm_webln_parse_sats(JsonNode *n, guint64 *out) {
  if (!n || JSON_NODE_HOLDS_NULL(n)) return NM_SATS_ABSENT;
  if (!JSON_NODE_HOLDS_VALUE(n)) return NM_SATS_INVALID;
  GType t = json_node_get_value_type(n);
  guint64 v = 0;
  if (t == G_TYPE_INT64) {
    gint64 i = json_node_get_int(n);
    if (i < 0) return NM_SATS_INVALID;
    v = (guint64)i;
  } else if (t == G_TYPE_DOUBLE) {
    /* JSON.stringify(1e21) and friends: only exact integers are amounts */
    gdouble d = json_node_get_double(n);
    if (!(d >= 0)) return NM_SATS_INVALID;           /* negative or NaN */
    if (d > 1e15) return NM_SATS_TOO_LARGE;
    if (d != (gdouble)(gint64)d) return NM_SATS_INVALID; /* fractional */
    v = (guint64)d;
  } else if (t == G_TYPE_STRING) {
    const gchar *s = json_node_get_string(n);
    gsize len = s ? strlen(s) : 0;
    if (len == 0) return NM_SATS_INVALID;
    for (gsize i = 0; i < len; i++)
      if (!g_ascii_isdigit(s[i])) return NM_SATS_INVALID;
    /* skip leading zeros, then anything longer than 15 digits is too large */
    while (len > 1 && *s == '0') { s++; len--; }
    if (len > 15) return NM_SATS_TOO_LARGE;
    v = g_ascii_strtoull(s, NULL, 10);
  } else {
    return NM_SATS_INVALID;
  }
  if (v > NM_WEBLN_MAX_SATS) return NM_SATS_TOO_LARGE;
  *out = v;
  return NM_SATS_OK;
}

gboolean nm_webln_sats_to_msat(guint64 sats, guint32 *out) {
  if (sats > NM_WEBLN_MAX_SATS) return FALSE;
  *out = (guint32)(sats * 1000u);
  return TRUE;
}

/* Parse one optional amount member; FALSE with code/why on a bad value. */
static gboolean opt_sats(JsonObject *p, const gchar *name, gboolean *present, guint64 *v,
                         NmErrorCode *code, const gchar **why) {
  *present = FALSE;
  switch (nm_webln_parse_sats(json_object_get_member(p, name), v)) {
    case NM_SATS_ABSENT:
      return TRUE;
    case NM_SATS_OK:
      *present = TRUE;
      return TRUE;
    case NM_SATS_TOO_LARGE:
      *code = NM_ERR_TOO_LARGE;
      *why = "Amount exceeds the wallet's per-call limit of 4294967 sats";
      return FALSE;
    case NM_SATS_INVALID:
    default:
      *code = NM_ERR_INVALID_REQUEST;
      *why = "Amounts must be non-negative integers (sats)";
      return FALSE;
  }
}

gboolean nm_webln_resolve_invoice(JsonObject *p, guint32 *out_msat, const gchar **out_memo,
                                  NmErrorCode *code, const gchar **why) {
  gboolean has_amt, has_def, has_min, has_max;
  guint64 amt = 0, def = 0, min = 0, max = 0;
  if (!opt_sats(p, "amount", &has_amt, &amt, code, why) ||
      !opt_sats(p, "defaultAmount", &has_def, &def, code, why) ||
      !opt_sats(p, "minimumAmount", &has_min, &min, code, why) ||
      !opt_sats(p, "maximumAmount", &has_max, &max, code, why))
    return FALSE;

  *code = NM_ERR_INVALID_REQUEST;
  if (has_min && has_max && min > max) {
    *why = "minimumAmount is greater than maximumAmount";
    return FALSE;
  }
  guint64 v = has_amt ? amt : has_def ? def : (has_min && min > 0) ? min : 0;
  if (v == 0) {
    *why = "An amount is required (amount-less invoices are not supported)";
    return FALSE;
  }
  if ((has_min && v < min) || (has_max && v > max)) {
    *why = "Amount is outside minimumAmount..maximumAmount";
    return FALSE;
  }

  const gchar *memo = "";
  JsonNode *mn = json_object_get_member(p, "defaultMemo");
  if (mn && !JSON_NODE_HOLDS_NULL(mn)) {
    if (!JSON_NODE_HOLDS_VALUE(mn) || json_node_get_value_type(mn) != G_TYPE_STRING) {
      *why = "defaultMemo must be a string";
      return FALSE;
    }
    memo = json_node_get_string(mn);
    if (strlen(memo) > NM_WEBLN_MAX_MEMO) {
      *why = "defaultMemo is longer than 639 bytes";
      return FALSE;
    }
  }
  if (!nm_webln_sats_to_msat(v, out_msat)) { /* unreachable: parse caps it */
    *code = NM_ERR_TOO_LARGE;
    *why = "Amount exceeds the wallet's per-call limit of 4294967 sats";
    return FALSE;
  }
  *out_memo = memo;
  return TRUE;
}

gboolean nm_webln_is_bolt11(const gchar *s) {
  static const gchar charset[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
  if (!s) return FALSE;
  gsize len = strlen(s);
  if (len < 20 || len > NM_WEBLN_MAX_BOLT11) return FALSE;
  if (g_ascii_strncasecmp(s, "ln", 2) != 0) return FALSE;
  if (g_ascii_strncasecmp(s, "lnurl", 5) == 0) return FALSE; /* LNURL is not an invoice */
  gboolean lower = FALSE, upper = FALSE;
  for (gsize i = 0; i < len; i++) {
    if (!g_ascii_isalnum(s[i])) return FALSE;
    if (g_ascii_islower(s[i])) lower = TRUE;
    if (g_ascii_isupper(s[i])) upper = TRUE;
  }
  if (lower && upper) return FALSE; /* bech32: one case only */
  const gchar *sep = strrchr(s, '1');
  if (!sep || sep - s < 4 || (gsize)(s + len - sep) < 8) return FALSE;
  for (const gchar *p = sep + 1; *p; p++)
    if (!strchr(charset, g_ascii_tolower(*p))) return FALSE;
  return TRUE;
}

static const gchar *dict_str(GVariant *d, const gchar *key) {
  const gchar *v = NULL;
  if (!g_variant_lookup(d, key, "&s", &v) || !v || !*v) return NULL;
  return v;
}

JsonNode *nm_webln_info_from_vardict(GVariant *info) {
  gboolean paired = FALSE;
  if (!info || !g_variant_is_of_type(info, G_VARIANT_TYPE_VARDICT) ||
      !g_variant_lookup(info, "paired", "b", &paired) || !paired)
    return NULL;

  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "node");
  json_builder_begin_object(b);
  const gchar *alias = dict_str(info, "alias");
  if (!alias) alias = dict_str(info, "lud16");
  json_builder_set_member_name(b, "alias");
  json_builder_add_string_value(b, alias ? alias : "");
  const gchar *pubkey = dict_str(info, "pubkey"); /* the node's, from get_info */
  if (pubkey) {
    json_builder_set_member_name(b, "pubkey");
    json_builder_add_string_value(b, pubkey);
  }
  const gchar *color = dict_str(info, "color");
  if (color) {
    json_builder_set_member_name(b, "color");
    json_builder_add_string_value(b, color);
  }
  json_builder_end_object(b);

  static const struct { const gchar *nip47, *webln; } map[] = {
    { "make_invoice", "makeInvoice" },
    { "pay_invoice",  "sendPayment" },
    { "get_balance",  "getBalance" },
  };
  g_autofree const gchar **methods = NULL;
  if (!g_variant_lookup(info, "methods", "^a&s", &methods)) methods = NULL;
  gboolean any = methods && methods[0];
  json_builder_set_member_name(b, "methods");
  json_builder_begin_array(b);
  json_builder_add_string_value(b, "getInfo");
  for (gsize i = 0; i < G_N_ELEMENTS(map); i++)
    if (!any || g_strv_contains((const gchar *const *)methods, map[i].nip47))
      json_builder_add_string_value(b, map[i].webln);
  json_builder_end_array(b);

  json_builder_set_member_name(b, "supports");
  json_builder_begin_array(b);
  json_builder_add_string_value(b, "lightning");
  json_builder_end_array(b);
  json_builder_set_member_name(b, "version");
  json_builder_add_string_value(b, "nostr-wallet-agent");
  json_builder_end_object(b);
  return json_builder_get_root(b);
}

/* The agent's message, stripped of the "GDBus.Error:<name>: " prefix and
 * bounded, when it is plain enough to show a page. */
static gchar *agent_message(const GError *error) {
  g_autoptr(GError) copy = g_error_copy(error);
  g_dbus_error_strip_remote_error(copy);
  const gchar *m = copy->message;
  if (!m || !*m || !g_utf8_validate(m, -1, NULL)) return NULL;
  if (g_utf8_strlen(m, -1) > 300) {
    g_autofree gchar *head = g_utf8_substring(m, 0, 300);
    return g_strconcat(head, "…", NULL);
  }
  return g_strdup(m);
}

NmErrorCode nm_webln_error_from_dbus(const GError *error, gchar **message) {
  *message = NULL;
  if (!error) return NM_ERR_INTERNAL;
  g_autofree gchar *remote = g_dbus_error_get_remote_error(error);
  static const gchar prefix[] = "org.nostr.Wallet1.Error.";
  if (remote && g_str_has_prefix(remote, prefix)) {
    const gchar *name = remote + strlen(prefix);
    g_autofree gchar *msg = agent_message(error);
    NmErrorCode code = NM_ERR_INTERNAL;
    gboolean pass = TRUE;
    if (!strcmp(name, "InvalidArgs"))          code = NM_ERR_INVALID_REQUEST;
    else if (!strcmp(name, "NotPaired"))       { code = NM_ERR_NOT_PAIRED; pass = FALSE; }
    else if (!strcmp(name, "Denied"))          code = NM_ERR_REJECTED;
    else if (!strcmp(name, "BudgetExceeded"))  code = NM_ERR_BUDGET_EXCEEDED;
    else if (!strcmp(name, "Timeout"))         { code = NM_ERR_TIMEOUT; pass = FALSE; }
    else if (!strcmp(name, "RelayError"))      code = NM_ERR_WALLET_UNAVAILABLE;
    else if (!strcmp(name, "Unsupported"))     code = NM_ERR_UNSUPPORTED;
    else if (!strcmp(name, "RateLimited"))     code = NM_ERR_RATE_LIMITED;
    else if (!strcmp(name, "WalletError")) {
      /* "[NIP47_CODE] text" */
      if (msg && g_str_has_prefix(msg, "[NOT_IMPLEMENTED]"))   code = NM_ERR_UNSUPPORTED;
      else if (msg && g_str_has_prefix(msg, "[RATE_LIMITED]")) code = NM_ERR_RATE_LIMITED;
      else if (msg && g_str_has_prefix(msg, "[RESTRICTED]"))   code = NM_ERR_REJECTED;
      else                                                     code = NM_ERR_WALLET_ERROR;
    } else {
      pass = FALSE; /* Keyring, Failed, anything new: internal */
    }
    if (pass) *message = g_steal_pointer(&msg);
    return code;
  }
  NmErrorCode code = nm_error_from_dbus(error);
  if (code == NM_ERR_SIGNER_UNAVAILABLE) return NM_ERR_WALLET_UNAVAILABLE;
  if (code == NM_ERR_UNSUPPORTED)
    *message = g_strdup("The wallet agent does not support per-site requests; update nostr-wallet-agent");
  return code;
}
