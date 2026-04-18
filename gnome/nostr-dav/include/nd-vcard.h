/* nd-vcard.h - Lightweight vCard 4.0 ↔ kind-30085 conversion
 *
 * SPDX-License-Identifier: MIT
 *
 * Parses vCard 4.0 contacts and converts them to/from kind-30085
 * Nostr events. No libebook dependency — handles the subset of
 * properties needed for the CardDAV bridge.
 */
#ifndef ND_VCARD_H
#define ND_VCARD_H

#include <glib.h>

G_BEGIN_DECLS

/* Application-specific kind for personal contacts */
#define ND_CONTACT_KIND 30085

/**
 * NdContact:
 *
 * Represents a contact in both vCard and Nostr domains.
 */
typedef struct {
  gchar *uid;           /* vCard UID / Nostr "d" tag */
  gchar *fn;            /* vCard FN (formatted name) / Nostr "name" tag */
  gchar *n;             /* vCard N (structured name: family;given;...) */
  gchar *email;         /* vCard EMAIL (first) */
  gchar *email_type;    /* EMAIL TYPE parameter (e.g. "work") */
  gchar *tel;           /* vCard TEL (first) */
  gchar *tel_type;      /* TEL TYPE parameter (e.g. "cell") */
  gchar *org;           /* vCard ORG */
  gchar *title;         /* vCard TITLE */
  gchar *adr;           /* vCard ADR (full semicolon-delimited) */
  gchar *adr_type;      /* ADR TYPE parameter */
  gchar *url;           /* vCard URL */
  gchar *note;          /* vCard NOTE */
  gchar *photo_uri;     /* vCard PHOTO (URI only, not inline data) */

  /* Nostr-specific */
  gchar *npub;          /* p-tag: contact's npub if known */
  gchar *pubkey;        /* Creator pubkey (hex) */
  gint64 created_at;    /* Event created_at */

  /* Raw vCard text (preserved for lossless round-trip) */
  gchar *raw_vcard;     /* Original vCard text if parsed from Nostr content */
} NdContact;

/**
 * nd_contact_free:
 * @contact: (transfer full): contact to free
 */
void nd_contact_free(NdContact *contact);

/**
 * nd_vcard_parse:
 * @vcard_text: raw vCard text
 * @error: (out) (optional): location for error
 *
 * Parses a vCard 4.0 (or 3.0) text into an NdContact.
 *
 * Returns: (transfer full) (nullable): parsed contact or NULL on error.
 */
NdContact *nd_vcard_parse(const gchar *vcard_text, GError **error);

/**
 * nd_vcard_generate:
 * @contact: the contact
 *
 * Generates a vCard 4.0 string from a contact.
 * If raw_vcard is set, returns that (lossless round-trip).
 *
 * Returns: (transfer full): vCard text.
 */
gchar *nd_vcard_generate(const NdContact *contact);

/**
 * nd_vcard_to_nostr_json:
 * @contact: the contact
 *
 * Builds unsigned kind-30085 event JSON from a contact.
 *
 * Returns: (transfer full) (nullable): JSON string.
 */
gchar *nd_vcard_to_nostr_json(const NdContact *contact);

/**
 * nd_vcard_from_nostr_json:
 * @json_str: kind-30085 event JSON
 * @error: (out) (optional): location for error
 *
 * Parses a kind-30085 event into an NdContact.
 *
 * Returns: (transfer full) (nullable): parsed contact or NULL on error.
 */
NdContact *nd_vcard_from_nostr_json(const gchar *json_str, GError **error);

/**
 * nd_vcard_compute_etag:
 * @contact: the contact
 *
 * Computes a stable ETag for a contact.
 *
 * Returns: (transfer full): ETag string (quoted).
 */
gchar *nd_vcard_compute_etag(const NdContact *contact);

G_END_DECLS
#endif /* ND_VCARD_H */
