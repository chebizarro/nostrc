/* nd-vcard.c - Lightweight vCard 4.0 ↔ kind-30085 conversion
 *
 * SPDX-License-Identifier: MIT
 */

#include "nd-vcard.h"

#include <json-glib/json-glib.h>
#include <string.h>
#include <time.h>

#define ND_VCARD_ERROR (nd_vcard_error_quark())
G_DEFINE_QUARK(nd-vcard-error-quark, nd_vcard_error)

enum {
  ND_VCARD_ERROR_PARSE = 1,
  ND_VCARD_ERROR_MISSING_FIELD
};

/* ---- Free ---- */

void
nd_contact_free(NdContact *contact)
{
  if (contact == NULL) return;
  g_free(contact->uid);
  g_free(contact->fn);
  g_free(contact->n);
  g_free(contact->email);
  g_free(contact->email_type);
  g_free(contact->tel);
  g_free(contact->tel_type);
  g_free(contact->org);
  g_free(contact->title);
  g_free(contact->adr);
  g_free(contact->adr_type);
  g_free(contact->url);
  g_free(contact->note);
  g_free(contact->photo_uri);
  g_free(contact->npub);
  g_free(contact->pubkey);
  g_free(contact->raw_vcard);
  g_free(contact);
}

/* ---- vCard line unfolding ---- */

/**
 * Unfold vCard content lines: CRLF followed by a space or tab is a
 * continuation of the previous line (same rule as ICS, RFC 6350 §3.2).
 */
static gchar *
vcard_unfold(const gchar *text)
{
  GString *out = g_string_new(NULL);
  const gchar *p = text;

  while (*p) {
    if (p[0] == '\r' && p[1] == '\n' && (p[2] == ' ' || p[2] == '\t')) {
      p += 3;
    } else if (p[0] == '\n' && (p[1] == ' ' || p[1] == '\t')) {
      p += 2;
    } else {
      g_string_append_c(out, *p);
      p++;
    }
  }

  return g_string_free(out, FALSE);
}

/**
 * Extract the TYPE parameter from a property line.
 * E.g. "EMAIL;TYPE=work:alice@example.com" → "work"
 */
static gchar *
extract_type_param(const gchar *line)
{
  const gchar *type = g_strstr_len(line, -1, "TYPE=");
  if (type == NULL) return NULL;

  type += 5;
  const gchar *end = strchr(type, ':');
  if (end == NULL) end = strchr(type, ';');
  if (end == NULL) return g_strdup(type);

  return g_strndup(type, (gsize)(end - type));
}

/**
 * Extract the value after the first ':' in a property line.
 */
static gchar *
extract_value(const gchar *line)
{
  const gchar *colon = strchr(line, ':');
  if (colon == NULL) return g_strdup("");
  return g_strdup(colon + 1);
}

/* ---- vCard parser ---- */

NdContact *
nd_vcard_parse(const gchar *vcard_text, GError **error)
{
  if (vcard_text == NULL || *vcard_text == '\0') {
    g_set_error_literal(error, ND_VCARD_ERROR, ND_VCARD_ERROR_PARSE,
                        "Empty vCard text");
    return NULL;
  }

  g_autofree gchar *unfolded = vcard_unfold(vcard_text);

  const gchar *vcard_start = strstr(unfolded, "BEGIN:VCARD");
  if (vcard_start == NULL) {
    g_set_error_literal(error, ND_VCARD_ERROR, ND_VCARD_ERROR_PARSE,
                        "No VCARD found");
    return NULL;
  }
  const gchar *vcard_end = strstr(vcard_start, "END:VCARD");
  if (vcard_end == NULL) {
    g_set_error_literal(error, ND_VCARD_ERROR, ND_VCARD_ERROR_PARSE,
                        "Unterminated VCARD");
    return NULL;
  }

  NdContact *contact = g_new0(NdContact, 1);
  /* Preserve raw vCard for lossless round-trip */
  contact->raw_vcard = g_strdup(vcard_text);

  g_autofree gchar *vcard_body = g_strndup(vcard_start,
                                            (gsize)(vcard_end - vcard_start + 9));
  g_auto(GStrv) lines = g_strsplit(vcard_body, "\n", -1);

  for (gsize i = 0; lines[i] != NULL; i++) {
    gchar *line = g_strstrip(lines[i]);
    gsize len = strlen(line);
    if (len > 0 && line[len - 1] == '\r')
      line[len - 1] = '\0';

    if (g_str_has_prefix(line, "UID:")) {
      g_free(contact->uid);
      contact->uid = extract_value(line);
    } else if (g_str_has_prefix(line, "FN:") || g_str_has_prefix(line, "FN;")) {
      g_free(contact->fn);
      contact->fn = extract_value(line);
    } else if (g_str_has_prefix(line, "N:") || g_str_has_prefix(line, "N;")) {
      g_free(contact->n);
      contact->n = extract_value(line);
    } else if (g_str_has_prefix(line, "EMAIL") &&
               (line[5] == ':' || line[5] == ';')) {
      if (contact->email == NULL) {
        contact->email = extract_value(line);
        contact->email_type = extract_type_param(line);
      }
    } else if (g_str_has_prefix(line, "TEL") &&
               (line[3] == ':' || line[3] == ';')) {
      if (contact->tel == NULL) {
        contact->tel = extract_value(line);
        contact->tel_type = extract_type_param(line);
      }
    } else if (g_str_has_prefix(line, "ORG:") || g_str_has_prefix(line, "ORG;")) {
      g_free(contact->org);
      contact->org = extract_value(line);
    } else if (g_str_has_prefix(line, "TITLE:") || g_str_has_prefix(line, "TITLE;")) {
      g_free(contact->title);
      contact->title = extract_value(line);
    } else if (g_str_has_prefix(line, "ADR") &&
               (line[3] == ':' || line[3] == ';')) {
      if (contact->adr == NULL) {
        contact->adr = extract_value(line);
        contact->adr_type = extract_type_param(line);
      }
    } else if (g_str_has_prefix(line, "URL:") || g_str_has_prefix(line, "URL;")) {
      g_free(contact->url);
      contact->url = extract_value(line);
    } else if (g_str_has_prefix(line, "NOTE:") || g_str_has_prefix(line, "NOTE;")) {
      g_free(contact->note);
      contact->note = extract_value(line);
    } else if (g_str_has_prefix(line, "PHOTO") &&
               (line[5] == ':' || line[5] == ';')) {
      /* Only store URI-based photos */
      if (strstr(line, "VALUE=uri") != NULL || strstr(line, "VALUE=URI") != NULL ||
          strstr(line, "http") != NULL) {
        g_free(contact->photo_uri);
        contact->photo_uri = extract_value(line);
      }
    }
  }

  if (contact->uid == NULL || contact->uid[0] == '\0') {
    g_set_error_literal(error, ND_VCARD_ERROR, ND_VCARD_ERROR_MISSING_FIELD,
                        "vCard missing required UID property");
    nd_contact_free(contact);
    return NULL;
  }

  if (contact->fn == NULL || contact->fn[0] == '\0') {
    g_set_error_literal(error, ND_VCARD_ERROR, ND_VCARD_ERROR_MISSING_FIELD,
                        "vCard missing required FN property");
    nd_contact_free(contact);
    return NULL;
  }

  return contact;
}

/* ---- vCard generator ---- */

gchar *
nd_vcard_generate(const NdContact *contact)
{
  g_return_val_if_fail(contact != NULL, NULL);

  /* Lossless round-trip: if we have the raw vCard, return it */
  if (contact->raw_vcard != NULL)
    return g_strdup(contact->raw_vcard);

  GString *vc = g_string_new(
    "BEGIN:VCARD\r\n"
    "VERSION:4.0\r\n");

  g_string_append_printf(vc, "UID:%s\r\n", contact->uid ? contact->uid : "");

  if (contact->fn)
    g_string_append_printf(vc, "FN:%s\r\n", contact->fn);

  if (contact->n)
    g_string_append_printf(vc, "N:%s\r\n", contact->n);

  if (contact->email) {
    if (contact->email_type)
      g_string_append_printf(vc, "EMAIL;TYPE=%s:%s\r\n",
                             contact->email_type, contact->email);
    else
      g_string_append_printf(vc, "EMAIL:%s\r\n", contact->email);
  }

  if (contact->tel) {
    if (contact->tel_type)
      g_string_append_printf(vc, "TEL;TYPE=%s:%s\r\n",
                             contact->tel_type, contact->tel);
    else
      g_string_append_printf(vc, "TEL:%s\r\n", contact->tel);
  }

  if (contact->org)
    g_string_append_printf(vc, "ORG:%s\r\n", contact->org);

  if (contact->title)
    g_string_append_printf(vc, "TITLE:%s\r\n", contact->title);

  if (contact->adr) {
    if (contact->adr_type)
      g_string_append_printf(vc, "ADR;TYPE=%s:%s\r\n",
                             contact->adr_type, contact->adr);
    else
      g_string_append_printf(vc, "ADR:%s\r\n", contact->adr);
  }

  if (contact->url)
    g_string_append_printf(vc, "URL:%s\r\n", contact->url);

  if (contact->note)
    g_string_append_printf(vc, "NOTE:%s\r\n", contact->note);

  if (contact->photo_uri)
    g_string_append_printf(vc, "PHOTO;VALUE=uri:%s\r\n", contact->photo_uri);

  g_string_append(vc, "END:VCARD\r\n");

  return g_string_free(vc, FALSE);
}

/* ---- Nostr JSON conversion ---- */

gchar *
nd_vcard_to_nostr_json(const NdContact *contact)
{
  g_return_val_if_fail(contact != NULL, NULL);
  g_return_val_if_fail(contact->uid != NULL, NULL);

  /* Generate vCard for content field */
  g_autofree gchar *vcard = nd_vcard_generate(contact);

  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);

  json_builder_set_member_name(b, "kind");
  json_builder_add_int_value(b, ND_CONTACT_KIND);

  json_builder_set_member_name(b, "content");
  json_builder_add_string_value(b, vcard ? vcard : "");

  json_builder_set_member_name(b, "created_at");
  json_builder_add_int_value(b, (gint64)time(NULL));

  json_builder_set_member_name(b, "tags");
  json_builder_begin_array(b);

  /* d tag (UID) */
  json_builder_begin_array(b);
  json_builder_add_string_value(b, "d");
  json_builder_add_string_value(b, contact->uid);
  json_builder_end_array(b);

  /* name tag */
  if (contact->fn) {
    json_builder_begin_array(b);
    json_builder_add_string_value(b, "name");
    json_builder_add_string_value(b, contact->fn);
    json_builder_end_array(b);
  }

  /* t tag */
  json_builder_begin_array(b);
  json_builder_add_string_value(b, "t");
  json_builder_add_string_value(b, "contact");
  json_builder_end_array(b);

  /* p tag (optional — contact's npub) */
  if (contact->npub) {
    json_builder_begin_array(b);
    json_builder_add_string_value(b, "p");
    json_builder_add_string_value(b, contact->npub);
    json_builder_end_array(b);
  }

  json_builder_end_array(b); /* tags */
  json_builder_end_object(b);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, json_builder_get_root(b));
  json_generator_set_pretty(gen, FALSE);
  return json_generator_to_data(gen, NULL);
}

NdContact *
nd_vcard_from_nostr_json(const gchar *json_str, GError **error)
{
  if (json_str == NULL || *json_str == '\0') {
    g_set_error_literal(error, ND_VCARD_ERROR, ND_VCARD_ERROR_PARSE,
                        "Empty JSON");
    return NULL;
  }

  g_autoptr(JsonParser) parser = json_parser_new();
  GError *parse_err = NULL;
  if (!json_parser_load_from_data(parser, json_str, -1, &parse_err)) {
    g_propagate_error(error, parse_err);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_set_error_literal(error, ND_VCARD_ERROR, ND_VCARD_ERROR_PARSE,
                        "JSON root is not an object");
    return NULL;
  }

  JsonObject *obj = json_node_get_object(root);
  gint64 kind = json_object_get_int_member_with_default(obj, "kind", 0);

  if (kind != ND_CONTACT_KIND) {
    g_set_error(error, ND_VCARD_ERROR, ND_VCARD_ERROR_PARSE,
                "Not a contact event (kind %" G_GINT64_FORMAT ")", kind);
    return NULL;
  }

  const gchar *content = json_object_get_string_member_with_default(
    obj, "content", "");

  /* Parse the vCard content */
  NdContact *contact = NULL;
  if (content && *content) {
    GError *vc_err = NULL;
    contact = nd_vcard_parse(content, &vc_err);
    if (contact == NULL) {
      /* If vCard parsing fails, create a minimal contact from tags */
      g_clear_error(&vc_err);
      contact = g_new0(NdContact, 1);
      contact->raw_vcard = g_strdup(content);
    }
  } else {
    contact = g_new0(NdContact, 1);
  }

  contact->created_at = json_object_get_int_member_with_default(
    obj, "created_at", 0);
  contact->pubkey = g_strdup(
    json_object_get_string_member_with_default(obj, "pubkey", ""));

  /* Parse tags for metadata */
  JsonArray *tags = json_object_get_array_member(obj, "tags");
  if (tags) {
    guint n = json_array_get_length(tags);
    for (guint i = 0; i < n; i++) {
      JsonArray *tag = json_array_get_array_element(tags, i);
      if (!tag || json_array_get_length(tag) < 2) continue;

      const gchar *key = json_array_get_string_element(tag, 0);
      const gchar *val = json_array_get_string_element(tag, 1);
      if (!key || !val) continue;

      if (g_str_equal(key, "d") && contact->uid == NULL) {
        contact->uid = g_strdup(val);
      } else if (g_str_equal(key, "name") && contact->fn == NULL) {
        contact->fn = g_strdup(val);
      } else if (g_str_equal(key, "p")) {
        g_free(contact->npub);
        contact->npub = g_strdup(val);
      }
    }
  }

  if (contact->uid == NULL) {
    g_set_error_literal(error, ND_VCARD_ERROR, ND_VCARD_ERROR_MISSING_FIELD,
                        "Contact event missing 'd' tag");
    nd_contact_free(contact);
    return NULL;
  }

  return contact;
}

/* ---- ETag ---- */

gchar *
nd_vcard_compute_etag(const NdContact *contact)
{
  g_return_val_if_fail(contact != NULL, NULL);

  g_autofree gchar *input = g_strdup_printf("%d:%s:%s:%" G_GINT64_FORMAT,
    ND_CONTACT_KIND,
    contact->pubkey ? contact->pubkey : "",
    contact->uid ? contact->uid : "",
    contact->created_at);

  g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(checksum, (const guchar *)input, strlen(input));
  const gchar *hash = g_checksum_get_string(checksum);

  return g_strdup_printf("\"%.*s\"", 16, hash);
}
