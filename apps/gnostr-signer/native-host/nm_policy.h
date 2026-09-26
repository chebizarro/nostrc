/* nm_policy.h - input policy for the NIP-07 bridge (nostrc-jjyp)
 *
 * Pure functions, no I/O, so each rule is unit-tested in isolation:
 *   - origin -> app_id (secure-origin policy + canonical form)
 *   - unsigned-event shape validation and canonicalization
 *   - hex / npub helpers and the NIP-07 getRelays result shape
 */
#ifndef APPS_GNOSTR_SIGNER_NATIVE_HOST_NM_POLICY_H
#define APPS_GNOSTR_SIGNER_NATIVE_HOST_NM_POLICY_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define NM_MAX_ORIGIN_LEN 512

/* Validate a web origin and return it as the app_id handed to
 * org.nostr.Signer.
 *
 * Accepts only tuple origins a browser would report for a secure context:
 *   https://<host>[:port]
 *   http://localhost[:port], http://<name>.localhost[:port],
 *   http://127.0.0.1[:port], http://[::1][:port]
 * and only in the browser's own serialization (lowercase scheme and host,
 * no default port, no trailing "/", punycode hosts): the input must be
 * byte-equal to its canonical form. The extension sends the browser's
 * serialized origin, so this makes the host a pure validator - the key the
 * extension gated on and the app_id the signer keys its ACL on can never
 * drift apart. Userinfo, path, query, fragment, percent-encoding,
 * non-ASCII and opaque origins ("null", file:, data:, extension schemes)
 * are refused.
 *
 * Returns the newly allocated app_id, or NULL with @why set to a short
 * static reason. */
gchar *nm_origin_to_app_id(const gchar *origin, const gchar **why);

/* Validate a NIP-07 unsigned event and rebuild it from the recognised
 * fields only: {"kind","created_at","tags","content"}.
 *   kind        integer, 0..65535
 *   created_at  integer >= 0 (absent, null or 0 -> @now; the signer would
 *               otherwise substitute its own clock)
 *   tags        array of arrays of strings
 *   content     string
 *   pubkey      optional; if present must be 64 hex (the signer's key wins)
 * id / sig and unknown members are dropped. Returns the canonical JSON or
 * NULL with @why set. */
gchar *nm_event_canonicalize(JsonNode *event, gint64 now, const gchar **why);

/* NIP-01 event id: lowercase hex SHA-256 of
 * [0,<pubkey>,<created_at>,<kind>,<tags>,<content>] serialized with the
 * NIP-01 escapes (\" \\ \b \t \n \f \r, other control bytes as \u00xx, like
 * libnostr). @escape_del selects whether 0x7f is escaped (libnostr) or
 * kept verbatim (JSON.stringify-style serializers). */
gchar *nm_event_id(const gchar *pubkey_hex, gint64 created_at, gint64 kind,
                   JsonNode *tags, const gchar *content, gboolean escape_del);

/* Check a signer reply against the canonical unsigned event that was sent:
 * kind, created_at, tags and content must be unchanged, pubkey 64 hex, sig
 * 128 hex, and id must be the NIP-01 hash of the returned fields. (The
 * Schnorr signature itself is left to the relay/client; the host links no
 * secp256k1.) Returns TRUE, or FALSE with @why set. */
gboolean nm_signed_event_matches(const gchar *canonical_unsigned, JsonNode *signed_event,
                                 const gchar **why);

/* TRUE iff @s is exactly 64 hex digits. */
gboolean nm_is_hex64(const gchar *s);

/* Lowercase copy of a 64-hex pubkey, or NULL. */
gchar *nm_pubkey_normalize(const gchar *s);

/* Decode an npub (bech32, NIP-19) to 64 lowercase hex, or NULL. A bare
 * 64-hex input is passed through lowercased. */
gchar *nm_pubkey_to_hex(const gchar *npub_or_hex);

/* Convert the daemon's GetRelays payload (JSON array of URLs) into the
 * NIP-07 getRelays shape {url: {"read": true, "write": true}}. An object
 * payload is passed through. Returns a new JsonNode or NULL. */
JsonNode *nm_relays_to_nip07(const gchar *relays_json);

/* TRUE when the raw JSON text contains a \u0000 escape. json-glib stores
 * strings NUL-terminated, so such a string would be silently truncated
 * before it reaches the signer - the host refuses the frame instead. */
gboolean nm_json_has_nul_escape(const gchar *json, gsize len);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_NATIVE_HOST_NM_POLICY_H */
