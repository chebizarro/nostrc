#include "gh-recipient.h"

#include <nostr-utils.h>
#include <nostr/nip19/nip19.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PASTE      2048 /* longer pastes are TEXT: nothing decodes them */
#define MAX_NIP05_LOCAL 64
#define MAX_DOMAIN     253
#define MAX_LABEL      63

gboolean
gh_recipient_is_pubkey(const gchar *value)
{
  if (!value || strlen(value) != 64)
    return FALSE;
  for (guint i = 0; i < 64; i++)
    if (!g_ascii_isxdigit(value[i]))
      return FALSE;
  return TRUE;
}

static gboolean
valid_label(const gchar *label, gsize len)
{
  if (len == 0 || len > MAX_LABEL || label[0] == '-' || label[len - 1] == '-')
    return FALSE;
  for (gsize i = 0; i < len; i++)
    if (!g_ascii_isalnum(label[i]) && label[i] != '-')
      return FALSE;
  return TRUE;
}

gboolean
gh_recipient_parse_nip05(const gchar *address, gchar **out_local, gchar **out_domain)
{
  if (!address)
    return FALSE;
  const gchar *at = strchr(address, '@');
  if (!at || strchr(at + 1, '@'))
    return FALSE;
  gsize local_len = at - address;
  const gchar *domain = at + 1;
  gsize domain_len = strlen(domain);
  if (local_len == 0 || local_len > MAX_NIP05_LOCAL || domain_len == 0 ||
      domain_len > MAX_DOMAIN)
    return FALSE;
  for (gsize i = 0; i < local_len; i++) {
    gchar c = address[i];
    if (!g_ascii_isalnum(c) && c != '-' && c != '_' && c != '.')
      return FALSE;
  }
  guint labels = 0;
  const gchar *last = NULL;
  gsize last_len = 0;
  for (const gchar *label = domain; label;) {
    const gchar *dot = strchr(label, '.');
    gsize len = dot ? (gsize)(dot - label) : strlen(label);
    if (!valid_label(label, len))
      return FALSE;
    labels++;
    last = label;
    last_len = len;
    label = dot ? dot + 1 : NULL;
  }
  /* A host name, not an IP literal: the top-level label has a letter. */
  gboolean has_alpha = FALSE;
  for (gsize i = 0; i < last_len; i++)
    has_alpha = has_alpha || g_ascii_isalpha(last[i]);
  if (labels < 2 || !has_alpha)
    return FALSE;
  if (out_local)
    *out_local = g_ascii_strdown(address, local_len);
  if (out_domain)
    *out_domain = g_ascii_strdown(domain, domain_len);
  return TRUE;
}

static gboolean
has_prefix_ci(const gchar *text, const gchar *prefix)
{
  return g_ascii_strncasecmp(text, prefix, strlen(prefix)) == 0;
}

static gchar *
hex_of(const guint8 bytes[32])
{
  gchar *hex = g_malloc(65);
  for (guint i = 0; i < 32; i++)
    g_snprintf(hex + 2 * i, 3, "%02x", bytes[i]);
  return hex;
}

/* A NIP-19 identifier (lower-cased) classified into input. */
static void
classify_bech32(GhRecipientInput *input, const gchar *bech)
{
  if (has_prefix_ci(bech, "nsec1") || has_prefix_ci(bech, "ncryptsec1")) {
    input->kind = GH_RECIPIENT_INPUT_SECRET;
    return;
  }
  NostrBech32Type type = NOSTR_B32_UNKNOWN;
  if (nostr_nip19_inspect(bech, &type) != 0) {
    input->kind = GH_RECIPIENT_INPUT_INVALID;
    return;
  }
  switch (type) {
  case NOSTR_B32_NPUB: {
    guint8 bytes[32];
    if (nostr_nip19_decode_npub(bech, bytes) != 0) {
      input->kind = GH_RECIPIENT_INPUT_INVALID;
      return;
    }
    input->pubkey = hex_of(bytes);
    input->kind = GH_RECIPIENT_INPUT_PUBKEY;
    return;
  }
  case NOSTR_B32_NPROFILE: {
    NostrProfilePointer *pointer = NULL;
    if (nostr_nip19_decode_nprofile(bech, &pointer) != 0 || !pointer ||
        !gh_recipient_is_pubkey(pointer->public_key)) {
      if (pointer)
        nostr_profile_pointer_free(pointer);
      input->kind = GH_RECIPIENT_INPUT_INVALID;
      return;
    }
    /* The relay hints are dropped (see the header). */
    input->pubkey = g_ascii_strdown(pointer->public_key, -1);
    nostr_profile_pointer_free(pointer);
    input->kind = GH_RECIPIENT_INPUT_PUBKEY;
    return;
  }
  case NOSTR_B32_NSEC:
    input->kind = GH_RECIPIENT_INPUT_SECRET;
    return;
  case NOSTR_B32_NOTE:
  case NOSTR_B32_NEVENT:
  case NOSTR_B32_NADDR:
  case NOSTR_B32_NRELAY:
    input->kind = GH_RECIPIENT_INPUT_OTHER_ENTITY;
    return;
  default:
    input->kind = GH_RECIPIENT_INPUT_INVALID;
    return;
  }
}

static gboolean
looks_like_bech32(const gchar *text)
{
  static const gchar *const prefixes[] = {
    "npub1", "nprofile1", "nsec1", "ncryptsec1", "note1", "nevent1", "naddr1", "nrelay1",
  };
  for (guint i = 0; i < G_N_ELEMENTS(prefixes); i++)
    if (has_prefix_ci(text, prefixes[i]))
      return TRUE;
  return FALSE;
}

GhRecipientInput *
gh_recipient_input_parse(const gchar *text)
{
  GhRecipientInput *input = g_new0(GhRecipientInput, 1);
  input->text = g_strstrip(g_strdup(text ? text : ""));
  const gchar *value = input->text;
  if (!*value) {
    input->kind = GH_RECIPIENT_INPUT_EMPTY;
    return input;
  }
  if (strlen(value) > MAX_PASTE || !g_utf8_validate(value, -1, NULL)) {
    input->kind = GH_RECIPIENT_INPUT_TEXT;
    return input;
  }
  if (has_prefix_ci(value, "bunker://")) {
    input->kind = GH_RECIPIENT_INPUT_SECRET;
    return input;
  }
  gboolean uri = has_prefix_ci(value, "nostr:");
  const gchar *rest = uri ? value + strlen("nostr:") : value;
  if (!uri && *rest == '@' && looks_like_bech32(rest + 1))
    rest++;
  if (uri || looks_like_bech32(rest)) {
    g_autofree gchar *bech = g_ascii_strdown(rest, -1);
    classify_bech32(input, bech);
    return input;
  }
  if (gh_recipient_parse_nip05(value, &input->nip05_local, &input->nip05_domain)) {
    input->nip05 = g_strconcat(input->nip05_local, "@", input->nip05_domain, NULL);
    input->kind = GH_RECIPIENT_INPUT_NIP05;
    return input;
  }
  input->kind = GH_RECIPIENT_INPUT_TEXT;
  return input;
}

void
gh_recipient_input_free(GhRecipientInput *input)
{
  if (!input)
    return;
  g_free(input->text);
  g_free(input->pubkey);
  g_free(input->nip05);
  g_free(input->nip05_local);
  g_free(input->nip05_domain);
  g_free(input);
}

gchar *
gh_recipient_npub(const gchar *pubkey_hex)
{
  guint8 bytes[32];
  char *npub = NULL;
  if (!gh_recipient_is_pubkey(pubkey_hex) || !nostr_hex2bin(bytes, pubkey_hex, sizeof bytes) ||
      nostr_nip19_encode_npub(bytes, &npub) != 0 || !npub)
    return NULL;
  gchar *copy = g_strdup(npub);
  free(npub);
  return copy;
}

gchar *
gh_recipient_npub_short(const gchar *pubkey_hex)
{
  g_autofree gchar *npub = gh_recipient_npub(pubkey_hex);
  if (!npub)
    return g_strdup(pubkey_hex ? pubkey_hex : "");
  gsize length = strlen(npub);
  return length > 16 ? g_strdup_printf("%.10s…%s", npub, npub + length - 4) : g_strdup(npub);
}

gchar *
gh_recipient_npub_grouped(const gchar *pubkey_hex)
{
  g_autofree gchar *npub = gh_recipient_npub(pubkey_hex);
  if (!npub)
    return NULL;
  const gsize hrp = strlen("npub1");
  GString *out = g_string_new_len(npub, hrp);
  for (const gchar *p = npub + hrp; *p; p += MIN(4, strlen(p))) {
    g_string_append_c(out, ' ');
    g_string_append_len(out, p, MIN(4, strlen(p)));
  }
  return g_string_free(out, FALSE);
}
