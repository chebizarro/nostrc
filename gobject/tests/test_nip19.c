/**
 * Unit tests for GNostrNip19 GObject wrapper (NIP-19 bech32 encoding/decoding)
 */

#include <glib.h>
#include "nostr_nip19.h"

/* Known test vector: a 32-byte zero key as hex */
static const gchar *TEST_PUBKEY_HEX =
  "3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d";
static const gchar *TEST_EVENT_ID_HEX =
  "b9f5441e45ca39179320e0031cfb18e34078673dcc3d3e3a3b3a981571b14f7c";

/* ── npub round-trip ─────────────────────────────────────────────── */

static void
test_npub_encode_decode(void)
{
  GError *error = NULL;

  /* Encode */
  g_autoptr(GNostrNip19) encoded = gnostr_nip19_encode_npub(TEST_PUBKEY_HEX, &error);
  g_assert_no_error(error);
  g_assert_nonnull(encoded);

  const gchar *bech32 = gnostr_nip19_get_bech32(encoded);
  g_assert_nonnull(bech32);
  g_assert_true(g_str_has_prefix(bech32, "npub1"));
  g_assert_cmpint(gnostr_nip19_get_entity_type(encoded), ==, GNOSTR_BECH32_NPUB);
  g_assert_cmpstr(gnostr_nip19_get_pubkey(encoded), ==, TEST_PUBKEY_HEX);

  /* Decode */
  g_autoptr(GNostrNip19) decoded = gnostr_nip19_decode(bech32, &error);
  g_assert_no_error(error);
  g_assert_nonnull(decoded);
  g_assert_cmpint(gnostr_nip19_get_entity_type(decoded), ==, GNOSTR_BECH32_NPUB);
  g_assert_cmpstr(gnostr_nip19_get_pubkey(decoded), ==, TEST_PUBKEY_HEX);
}

/* ── nsec round-trip ─────────────────────────────────────────────── */

static void
test_nsec_encode_decode(void)
{
  /* Use the same bytes as a secret key for testing */
  const gchar *seckey_hex = TEST_PUBKEY_HEX;
  GError *error = NULL;

  g_autoptr(GNostrNip19) encoded = gnostr_nip19_encode_nsec(seckey_hex, &error);
  g_assert_no_error(error);
  g_assert_nonnull(encoded);

  const gchar *bech32 = gnostr_nip19_get_bech32(encoded);
  g_assert_nonnull(bech32);
  g_assert_true(g_str_has_prefix(bech32, "nsec1"));
  g_assert_cmpint(gnostr_nip19_get_entity_type(encoded), ==, GNOSTR_BECH32_NSEC);

  /* Decode */
  g_autoptr(GNostrNip19) decoded = gnostr_nip19_decode(bech32, &error);
  g_assert_no_error(error);
  g_assert_nonnull(decoded);
  g_assert_cmpint(gnostr_nip19_get_entity_type(decoded), ==, GNOSTR_BECH32_NSEC);
  /* nsec doesn't populate pubkey - only hex */
  g_assert_null(gnostr_nip19_get_pubkey(decoded));
}

/* ── note round-trip ─────────────────────────────────────────────── */

static void
test_note_encode_decode(void)
{
  GError *error = NULL;

  g_autoptr(GNostrNip19) encoded = gnostr_nip19_encode_note(TEST_EVENT_ID_HEX, &error);
  g_assert_no_error(error);
  g_assert_nonnull(encoded);

  const gchar *bech32 = gnostr_nip19_get_bech32(encoded);
  g_assert_nonnull(bech32);
  g_assert_true(g_str_has_prefix(bech32, "note1"));
  g_assert_cmpint(gnostr_nip19_get_entity_type(encoded), ==, GNOSTR_BECH32_NOTE);
  g_assert_cmpstr(gnostr_nip19_get_event_id(encoded), ==, TEST_EVENT_ID_HEX);

  /* Decode */
  g_autoptr(GNostrNip19) decoded = gnostr_nip19_decode(bech32, &error);
  g_assert_no_error(error);
  g_assert_nonnull(decoded);
  g_assert_cmpint(gnostr_nip19_get_entity_type(decoded), ==, GNOSTR_BECH32_NOTE);
  g_assert_cmpstr(gnostr_nip19_get_event_id(decoded), ==, TEST_EVENT_ID_HEX);
}

/* ── nprofile round-trip ─────────────────────────────────────────── */

static void
test_nprofile_encode_decode(void)
{
  GError *error = NULL;
  const gchar *relays[] = { "wss://relay.damus.io", "wss://nos.lol", NULL };

  g_autoptr(GNostrNip19) encoded = gnostr_nip19_encode_nprofile(
    TEST_PUBKEY_HEX, relays, &error);
  g_assert_no_error(error);
  g_assert_nonnull(encoded);

  const gchar *bech32 = gnostr_nip19_get_bech32(encoded);
  g_assert_nonnull(bech32);
  g_assert_true(g_str_has_prefix(bech32, "nprofile1"));
  g_assert_cmpint(gnostr_nip19_get_entity_type(encoded), ==, GNOSTR_BECH32_NPROFILE);
  g_assert_cmpstr(gnostr_nip19_get_pubkey(encoded), ==, TEST_PUBKEY_HEX);

  const gchar *const *enc_relays = gnostr_nip19_get_relays(encoded);
  g_assert_nonnull(enc_relays);
  g_assert_cmpstr(enc_relays[0], ==, "wss://relay.damus.io");
  g_assert_cmpstr(enc_relays[1], ==, "wss://nos.lol");
  g_assert_null(enc_relays[2]);

  /* Decode */
  g_autoptr(GNostrNip19) decoded = gnostr_nip19_decode(bech32, &error);
  g_assert_no_error(error);
  g_assert_nonnull(decoded);
  g_assert_cmpint(gnostr_nip19_get_entity_type(decoded), ==, GNOSTR_BECH32_NPROFILE);
  g_assert_cmpstr(gnostr_nip19_get_pubkey(decoded), ==, TEST_PUBKEY_HEX);

  const gchar *const *dec_relays = gnostr_nip19_get_relays(decoded);
  g_assert_nonnull(dec_relays);
  g_assert_cmpstr(dec_relays[0], ==, "wss://relay.damus.io");
  g_assert_cmpstr(dec_relays[1], ==, "wss://nos.lol");
}

/* ── nprofile without relays ─────────────────────────────────────── */

static void
test_nprofile_no_relays(void)
{
  GError *error = NULL;

  g_autoptr(GNostrNip19) encoded = gnostr_nip19_encode_nprofile(
    TEST_PUBKEY_HEX, NULL, &error);
  g_assert_no_error(error);
  g_assert_nonnull(encoded);

  g_assert_true(g_str_has_prefix(gnostr_nip19_get_bech32(encoded), "nprofile1"));
  g_assert_cmpstr(gnostr_nip19_get_pubkey(encoded), ==, TEST_PUBKEY_HEX);
  g_assert_null(gnostr_nip19_get_relays(encoded));
}

/* ── nevent round-trip ───────────────────────────────────────────── */

static void
test_nevent_encode_decode(void)
{
  GError *error = NULL;
  const gchar *relays[] = { "wss://relay.damus.io", NULL };

  g_autoptr(GNostrNip19) encoded = gnostr_nip19_encode_nevent(
    TEST_EVENT_ID_HEX, relays, TEST_PUBKEY_HEX, 1, &error);
  g_assert_no_error(error);
  g_assert_nonnull(encoded);

  const gchar *bech32 = gnostr_nip19_get_bech32(encoded);
  g_assert_nonnull(bech32);
  g_assert_true(g_str_has_prefix(bech32, "nevent1"));
  g_assert_cmpint(gnostr_nip19_get_entity_type(encoded), ==, GNOSTR_BECH32_NEVENT);
  g_assert_cmpstr(gnostr_nip19_get_event_id(encoded), ==, TEST_EVENT_ID_HEX);
  g_assert_cmpstr(gnostr_nip19_get_author(encoded), ==, TEST_PUBKEY_HEX);
  g_assert_cmpint(gnostr_nip19_get_kind(encoded), ==, 1);

  /* Decode */
  g_autoptr(GNostrNip19) decoded = gnostr_nip19_decode(bech32, &error);
  g_assert_no_error(error);
  g_assert_nonnull(decoded);
  g_assert_cmpint(gnostr_nip19_get_entity_type(decoded), ==, GNOSTR_BECH32_NEVENT);
  g_assert_cmpstr(gnostr_nip19_get_event_id(decoded), ==, TEST_EVENT_ID_HEX);
  g_assert_cmpstr(gnostr_nip19_get_author(decoded), ==, TEST_PUBKEY_HEX);
  g_assert_cmpint(gnostr_nip19_get_kind(decoded), ==, 1);

  const gchar *const *dec_relays = gnostr_nip19_get_relays(decoded);
  g_assert_nonnull(dec_relays);
  g_assert_cmpstr(dec_relays[0], ==, "wss://relay.damus.io");
}

/* ── naddr round-trip ────────────────────────────────────────────── */

static void
test_naddr_encode_decode(void)
{
  GError *error = NULL;
  const gchar *relays[] = { "wss://relay.nostr.band", NULL };

  g_autoptr(GNostrNip19) encoded = gnostr_nip19_encode_naddr(
    "my-article", TEST_PUBKEY_HEX, 30023, relays, &error);
  g_assert_no_error(error);
  g_assert_nonnull(encoded);

  const gchar *bech32 = gnostr_nip19_get_bech32(encoded);
  g_assert_nonnull(bech32);
  g_assert_true(g_str_has_prefix(bech32, "naddr1"));
  g_assert_cmpint(gnostr_nip19_get_entity_type(encoded), ==, GNOSTR_BECH32_NADDR);
  g_assert_cmpstr(gnostr_nip19_get_identifier(encoded), ==, "my-article");
  g_assert_cmpstr(gnostr_nip19_get_pubkey(encoded), ==, TEST_PUBKEY_HEX);
  g_assert_cmpstr(gnostr_nip19_get_author(encoded), ==, TEST_PUBKEY_HEX);
  g_assert_cmpint(gnostr_nip19_get_kind(encoded), ==, 30023);

  /* Decode */
  g_autoptr(GNostrNip19) decoded = gnostr_nip19_decode(bech32, &error);
  g_assert_no_error(error);
  g_assert_nonnull(decoded);
  g_assert_cmpint(gnostr_nip19_get_entity_type(decoded), ==, GNOSTR_BECH32_NADDR);
  g_assert_cmpstr(gnostr_nip19_get_identifier(decoded), ==, "my-article");
  g_assert_cmpstr(gnostr_nip19_get_pubkey(decoded), ==, TEST_PUBKEY_HEX);
  g_assert_cmpint(gnostr_nip19_get_kind(decoded), ==, 30023);

  const gchar *const *dec_relays = gnostr_nip19_get_relays(decoded);
  g_assert_nonnull(dec_relays);
  g_assert_cmpstr(dec_relays[0], ==, "wss://relay.nostr.band");
}

/* ── nrelay round-trip ───────────────────────────────────────────── */

static void
test_nrelay_encode_decode(void)
{
  GError *error = NULL;
  const gchar *relays[] = { "wss://relay.damus.io", "wss://nos.lol", NULL };

  g_autoptr(GNostrNip19) encoded = gnostr_nip19_encode_nrelay(relays, &error);
  g_assert_no_error(error);
  g_assert_nonnull(encoded);

  const gchar *bech32 = gnostr_nip19_get_bech32(encoded);
  g_assert_nonnull(bech32);
  g_assert_true(g_str_has_prefix(bech32, "nrelay1"));
  g_assert_cmpint(gnostr_nip19_get_entity_type(encoded), ==, GNOSTR_BECH32_NRELAY);

  const gchar *const *enc_relays = gnostr_nip19_get_relays(encoded);
  g_assert_nonnull(enc_relays);
  g_assert_cmpstr(enc_relays[0], ==, "wss://relay.damus.io");
  g_assert_cmpstr(enc_relays[1], ==, "wss://nos.lol");

  /* Decode */
  g_autoptr(GNostrNip19) decoded = gnostr_nip19_decode(bech32, &error);
  g_assert_no_error(error);
  g_assert_nonnull(decoded);
  g_assert_cmpint(gnostr_nip19_get_entity_type(decoded), ==, GNOSTR_BECH32_NRELAY);

  const gchar *const *dec_relays = gnostr_nip19_get_relays(decoded);
  g_assert_nonnull(dec_relays);
  g_assert_cmpstr(dec_relays[0], ==, "wss://relay.damus.io");
  g_assert_cmpstr(dec_relays[1], ==, "wss://nos.lol");
}

/* ── inspect ─────────────────────────────────────────────────────── */

static void
test_inspect(void)
{
  GError *error = NULL;

  /* Create an npub to inspect */
  g_autoptr(GNostrNip19) npub = gnostr_nip19_encode_npub(TEST_PUBKEY_HEX, &error);
  g_assert_no_error(error);
  g_assert_cmpint(gnostr_nip19_inspect(gnostr_nip19_get_bech32(npub)),
                   ==, GNOSTR_BECH32_NPUB);

  /* Invalid input */
  g_assert_cmpint(gnostr_nip19_inspect("garbage"), ==, GNOSTR_BECH32_UNKNOWN);
  g_assert_cmpint(gnostr_nip19_inspect(""), ==, GNOSTR_BECH32_UNKNOWN);
}

/* ── Error handling ──────────────────────────────────────────────── */

static void
test_decode_invalid(void)
{
  GError *error = NULL;

  GNostrNip19 *result = gnostr_nip19_decode("not_a_bech32_string", &error);
  g_assert_null(result);
  g_assert_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED);
  g_clear_error(&error);

  result = gnostr_nip19_decode("npub1invalidchecksum", &error);
  g_assert_null(result);
  g_assert_nonnull(error);
  g_clear_error(&error);
}

static void
test_encode_invalid_hex(void)
{
  GError *error = NULL;

  /* Too short */
  GNostrNip19 *result = gnostr_nip19_encode_npub("abcd", &error);
  g_assert_null(result);
  g_assert_error(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_KEY);
  g_clear_error(&error);

  /* Non-hex characters */
  result = gnostr_nip19_encode_npub(
    "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", &error);
  g_assert_null(result);
  g_assert_error(error, NOSTR_ERROR, NOSTR_ERROR_INVALID_KEY);
  g_clear_error(&error);
}

static void
test_naddr_requires_kind(void)
{
  GError *error = NULL;

  GNostrNip19 *result = gnostr_nip19_encode_naddr(
    "test", TEST_PUBKEY_HEX, -1, NULL, &error);
  g_assert_null(result);
  g_assert_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED);
  g_clear_error(&error);
}

static void
test_nrelay_requires_relays(void)
{
  GError *error = NULL;
  const gchar *empty[] = { NULL };

  GNostrNip19 *result = gnostr_nip19_encode_nrelay(empty, &error);
  g_assert_null(result);
  g_assert_error(error, NOSTR_ERROR, NOSTR_ERROR_PARSE_FAILED);
  g_clear_error(&error);
}

/* ── GObject property access ─────────────────────────────────────── */

static void
test_gobject_properties(void)
{
  GError *error = NULL;
  const gchar *relays[] = { "wss://relay.damus.io", NULL };

  g_autoptr(GNostrNip19) obj = gnostr_nip19_encode_nprofile(
    TEST_PUBKEY_HEX, relays, &error);
  g_assert_no_error(error);
  g_assert_nonnull(obj);

  /* Access via g_object_get */
  gint entity_type;
  g_autofree gchar *bech32 = NULL;
  g_autofree gchar *pubkey = NULL;
  g_auto(GStrv) relay_strv = NULL;

  g_object_get(obj,
    "entity-type", &entity_type,
    "bech32", &bech32,
    "pubkey", &pubkey,
    "relays", &relay_strv,
    NULL);

  g_assert_cmpint(entity_type, ==, GNOSTR_BECH32_NPROFILE);
  g_assert_nonnull(bech32);
  g_assert_true(g_str_has_prefix(bech32, "nprofile1"));
  g_assert_cmpstr(pubkey, ==, TEST_PUBKEY_HEX);
  g_assert_nonnull(relay_strv);
  g_assert_cmpstr(relay_strv[0], ==, "wss://relay.damus.io");
}

/* ── Inapplicable fields return NULL/-1 ──────────────────────────── */

static void
test_inapplicable_fields(void)
{
  GError *error = NULL;

  g_autoptr(GNostrNip19) npub = gnostr_nip19_encode_npub(TEST_PUBKEY_HEX, &error);
  g_assert_no_error(error);

  /* npub has no event_id, author, kind, identifier, relays */
  g_assert_null(gnostr_nip19_get_event_id(npub));
  g_assert_null(gnostr_nip19_get_author(npub));
  g_assert_cmpint(gnostr_nip19_get_kind(npub), ==, -1);
  g_assert_null(gnostr_nip19_get_identifier(npub));
  g_assert_null(gnostr_nip19_get_relays(npub));
}

/* ── Enum type registration ──────────────────────────────────────── */

static void
test_bech32_type_enum(void)
{
  GType type = GNOSTR_TYPE_BECH32_TYPE;
  g_assert_true(G_TYPE_IS_ENUM(type));

  GEnumClass *klass = g_type_class_ref(type);
  g_assert_nonnull(klass);

  GEnumValue *val = g_enum_get_value(klass, GNOSTR_BECH32_NPUB);
  g_assert_nonnull(val);
  g_assert_cmpstr(val->value_nick, ==, "npub");

  val = g_enum_get_value(klass, GNOSTR_BECH32_NPROFILE);
  g_assert_nonnull(val);
  g_assert_cmpstr(val->value_nick, ==, "nprofile");

  g_type_class_unref(klass);
}

/* ── Main ────────────────────────────────────────────────────────── */

int
main(int argc, char *argv[])
{
  g_test_init(&argc, &argv, NULL);

  g_test_add_func("/nip19/npub-roundtrip", test_npub_encode_decode);
  g_test_add_func("/nip19/nsec-roundtrip", test_nsec_encode_decode);
  g_test_add_func("/nip19/note-roundtrip", test_note_encode_decode);
  g_test_add_func("/nip19/nprofile-roundtrip", test_nprofile_encode_decode);
  g_test_add_func("/nip19/nprofile-no-relays", test_nprofile_no_relays);
  g_test_add_func("/nip19/nevent-roundtrip", test_nevent_encode_decode);
  g_test_add_func("/nip19/naddr-roundtrip", test_naddr_encode_decode);
  g_test_add_func("/nip19/nrelay-roundtrip", test_nrelay_encode_decode);
  g_test_add_func("/nip19/inspect", test_inspect);
  g_test_add_func("/nip19/decode-invalid", test_decode_invalid);
  g_test_add_func("/nip19/encode-invalid-hex", test_encode_invalid_hex);
  g_test_add_func("/nip19/naddr-requires-kind", test_naddr_requires_kind);
  g_test_add_func("/nip19/nrelay-requires-relays", test_nrelay_requires_relays);
  g_test_add_func("/nip19/gobject-properties", test_gobject_properties);
  g_test_add_func("/nip19/inapplicable-fields", test_inapplicable_fields);
  g_test_add_func("/nip19/bech32-type-enum", test_bech32_type_enum);

  return g_test_run();
}
