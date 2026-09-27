/* NIP-17 kind-15 decryption-key / decryption-nonce encoding (nostrc-qh4j).
 *
 * NIP-17 does not name an encoding for these tags. Amethyst (quartz
 * EncryptionKey/EncryptionNonce: toHexKey / hexToByteArray), 0xchat
 * (aes_encrypt_utils.dart: hexToBytes) and nostr-share --private write
 * lower-case hex, and Amethyst/0xchat default to a 16-byte GCM nonce.
 * gnostr used to write and read base64 only. It now writes hex and reads hex
 * or its own legacy base64, with 12- or 16-byte nonces. */
#include "util/dm_files.h"

#include <glib.h>
#include <json-glib/json-glib.h>
#include <string.h>

static const uint8_t k_key[32] = {
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
  0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};
static const uint8_t k_nonce16[16] = {
  0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
};

#define KEY_HEX "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
#define NONCE12_HEX "a0a1a2a3a4a5a6a7a8a9aaab"
#define NONCE16_HEX "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf"
#define KEY_B64 "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8="
#define NONCE12_B64 "oKGio6Slpqeoqaqr"
#define NONCE16_B64 "oKGio6SlpqeoqaqrrK2urw=="

/* AES-256-GCM(key=k_key, nonce=k_nonce16) of PLAIN, ciphertext||tag.
 * Produced independently by Node crypto ("aes-256-gcm", 16-byte IV) and Go
 * cipher.NewGCMWithNonceSize(16) (identical output), i.e. what an Amethyst or
 * 0xchat sender uploads. */
#define PLAIN "kind-15 interop: amethyst/0xchat 16-byte nonce\n"
#define CT16_HEX                                                         \
  "41ca4dd835fcf856aaf06bd49f7bfdaaa2242e1c0a5c28df893d5ace7a5d47f32ca0d0" \
  "4ef7aacbd8ce675fcc7b27bde7211e6286028720d314fa489bb9d31d"

static GBytes *
unhex(const char *s)
{
  gsize n = strlen(s) / 2;
  guint8 *b = g_malloc(n);
  for (gsize i = 0; i < n; i++)
    b[i] = (guint8)((g_ascii_xdigit_value(s[2 * i]) << 4) | g_ascii_xdigit_value(s[2 * i + 1]));
  return g_bytes_new_take(b, n);
}

static char *
rumor_with(const char *key, const char *nonce)
{
  return g_strdup_printf(
      "{\"pubkey\":\"%064d\",\"created_at\":1,\"kind\":15,"
      "\"content\":\"https://blossom.example/abc\",\"tags\":["
      "[\"p\",\"%064d\"],[\"file-type\",\"text/plain\"],"
      "[\"encryption-algorithm\",\"aes-gcm\"],"
      "[\"decryption-key\",\"%s\"],[\"decryption-nonce\",\"%s\"]]}",
      1, 2, key, nonce);
}

static const char *
tag_value(JsonArray *tags, const char *name)
{
  for (guint i = 0; i < json_array_get_length(tags); i++) {
    JsonArray *t = json_array_get_array_element(tags, i);
    if (json_array_get_length(t) >= 2 &&
        g_strcmp0(json_array_get_string_element(t, 0), name) == 0)
      return json_array_get_string_element(t, 1);
  }
  return NULL;
}

/* gnostr now writes lower-case hex, and reads its own output back. */
static void
test_write_hex(void)
{
  GnostrDmFileAttachment att = { 0 };
  memcpy(att.key, k_key, sizeof(att.key));
  memcpy(att.nonce, k_nonce16, GNOSTR_DM_FILES_AES_NONCE_SIZE);
  att.upload_url = (char *)"https://blossom.example/abc";
  att.mime_type = (char *)"image/png";

  g_autofree char *json = gnostr_dm_file_build_rumor_json(
      "1111111111111111111111111111111111111111111111111111111111111111",
      "2222222222222222222222222222222222222222222222222222222222222222", &att, 1);
  g_assert_nonnull(json);

  g_autoptr(JsonParser) p = json_parser_new();
  g_assert_true(json_parser_load_from_data(p, json, -1, NULL));
  JsonArray *tags = json_object_get_array_member(json_node_get_object(json_parser_get_root(p)), "tags");
  g_assert_cmpstr(tag_value(tags, "decryption-key"), ==, KEY_HEX);
  g_assert_cmpstr(tag_value(tags, "decryption-nonce"), ==, NONCE12_HEX);

  GnostrDmFileMessage *msg = gnostr_dm_file_parse_message(json);
  g_assert_nonnull(msg);
  uint8_t key[GNOSTR_DM_FILES_AES_KEY_SIZE];
  uint8_t nonce[GNOSTR_DM_FILES_AES_NONCE_MAX_SIZE];
  gsize nonce_len = 0;
  g_assert_true(gnostr_dm_file_decode_key(msg->decryption_key, key));
  g_assert_true(gnostr_dm_file_decode_nonce(msg->decryption_nonce, nonce, &nonce_len));
  g_assert_cmpmem(key, sizeof(key), k_key, sizeof(k_key));
  g_assert_cmpmem(nonce, nonce_len, k_nonce16, GNOSTR_DM_FILES_AES_NONCE_SIZE);
  gnostr_dm_file_message_free(msg);
}

/* An Amethyst / 0xchat kind-15 (hex key, hex 16-byte nonce) opens. This is
 * the case the bug describes: before the fix the 64-char hex key went through
 * base64 and came out 48 bytes, and a 16-byte nonce was refused outright. */
static void
test_read_hex_16_byte_nonce_decrypts(void)
{
  g_autofree char *json = rumor_with(KEY_HEX, NONCE16_HEX);
  GnostrDmFileMessage *msg = gnostr_dm_file_parse_message(json);
  g_assert_nonnull(msg);

  uint8_t key[GNOSTR_DM_FILES_AES_KEY_SIZE];
  uint8_t nonce[GNOSTR_DM_FILES_AES_NONCE_MAX_SIZE];
  gsize nonce_len = 0;
  g_assert_true(gnostr_dm_file_decode_key(msg->decryption_key, key));
  g_assert_true(gnostr_dm_file_decode_nonce(msg->decryption_nonce, nonce, &nonce_len));
  g_assert_cmpuint(nonce_len, ==, 16);

  g_autoptr(GBytes) ct = unhex(CT16_HEX);
  gsize ct_len = 0;
  const uint8_t *ctp = g_bytes_get_data(ct, &ct_len);
  uint8_t out[128];
  gsize out_len = 0;
  g_assert_true(gnostr_dm_file_aes_gcm_decrypt(ctp, ct_len, key, nonce, nonce_len, out, &out_len));
  g_assert_cmpmem(out, out_len, PLAIN, strlen(PLAIN));

  /* Tampered tag still fails authentication. */
  uint8_t bad[128];
  memcpy(bad, ctp, ct_len);
  bad[ct_len - 1] ^= 1;
  g_assert_false(gnostr_dm_file_aes_gcm_decrypt(bad, ct_len, key, nonce, nonce_len, out, &out_len));
  gnostr_dm_file_message_free(msg);
}

/* nostr-share --private writes hex with a 12-byte nonce; round-trip through
 * gnostr's own encrypt. Upper-case hex is accepted too. */
static void
test_read_hex_12_byte_nonce_roundtrip(void)
{
  uint8_t key[GNOSTR_DM_FILES_AES_KEY_SIZE];
  uint8_t nonce[GNOSTR_DM_FILES_AES_NONCE_MAX_SIZE];
  gsize nonce_len = 0;
  g_assert_true(gnostr_dm_file_decode_key(KEY_HEX, key));
  g_assert_true(gnostr_dm_file_decode_nonce("A0A1A2A3A4A5A6A7A8A9AAAB", nonce, &nonce_len));
  g_assert_cmpuint(nonce_len, ==, GNOSTR_DM_FILES_AES_NONCE_SIZE);
  g_assert_cmpmem(nonce, nonce_len, k_nonce16, nonce_len);

  uint8_t ct[64 + GNOSTR_DM_FILES_AES_TAG_SIZE], pt[64];
  gsize ct_len = 0, pt_len = 0;
  g_assert_true(gnostr_dm_file_aes_gcm_encrypt((const uint8_t *)PLAIN, strlen(PLAIN), key, nonce, ct, &ct_len));
  g_assert_true(gnostr_dm_file_aes_gcm_decrypt(ct, ct_len, key, nonce, nonce_len, pt, &pt_len));
  g_assert_cmpmem(pt, pt_len, PLAIN, strlen(PLAIN));
}

/* Messages sent by gnostr before this change carry base64; keep opening them. */
static void
test_read_legacy_base64(void)
{
  uint8_t key[GNOSTR_DM_FILES_AES_KEY_SIZE];
  uint8_t nonce[GNOSTR_DM_FILES_AES_NONCE_MAX_SIZE];
  gsize nonce_len = 0;

  g_autofree char *json = rumor_with(KEY_B64, NONCE12_B64);
  GnostrDmFileMessage *msg = gnostr_dm_file_parse_message(json);
  g_assert_nonnull(msg);
  g_assert_true(gnostr_dm_file_decode_key(msg->decryption_key, key));
  g_assert_cmpmem(key, sizeof(key), k_key, sizeof(k_key));
  g_assert_true(gnostr_dm_file_decode_nonce(msg->decryption_nonce, nonce, &nonce_len));
  g_assert_cmpmem(nonce, nonce_len, k_nonce16, GNOSTR_DM_FILES_AES_NONCE_SIZE);
  gnostr_dm_file_message_free(msg);

  g_assert_true(gnostr_dm_file_decode_nonce(NONCE16_B64, nonce, &nonce_len));
  g_assert_cmpmem(nonce, nonce_len, k_nonce16, sizeof(k_nonce16));
}

static void
test_reject_malformed(void)
{
  uint8_t key[GNOSTR_DM_FILES_AES_KEY_SIZE];
  uint8_t nonce[GNOSTR_DM_FILES_AES_NONCE_MAX_SIZE];
  gsize nonce_len = 0;

  g_assert_false(gnostr_dm_file_decode_key(NULL, key));
  g_assert_false(gnostr_dm_file_decode_key("", key));
  /* 63 / 62 hex digits, non-hex digit, embedded junk in base64 */
  g_assert_false(gnostr_dm_file_decode_key(&KEY_HEX[1], key));
  g_assert_false(gnostr_dm_file_decode_key(&KEY_HEX[2], key));
  g_assert_false(gnostr_dm_file_decode_key("g00102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", key));
  g_assert_false(gnostr_dm_file_decode_key("AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwd*h8=", key));
  /* base64 of the wrong length (the 48 bytes a hex key decodes to as base64) */
  g_assert_false(gnostr_dm_file_decode_key(NONCE16_B64, key));
  /* a key is not a nonce and vice versa */
  g_assert_false(gnostr_dm_file_decode_nonce(KEY_HEX, nonce, &nonce_len));
  g_assert_false(gnostr_dm_file_decode_key(NONCE16_HEX, key));
  /* 10-byte hex nonce (as base64 it would be 15 bytes: no reading fits).
   * Note 16 hex-looking characters are NOT rejected: that is a valid legacy
   * base64 12-byte nonce, and hex of 8 bytes is never a valid nonce. */
  g_assert_false(gnostr_dm_file_decode_nonce("a0a1a2a3a4a5a6a7a8a9", nonce, &nonce_len));

  uint8_t ct[32] = { 0 }, pt[32];
  gsize pt_len = 0;
  g_assert_false(gnostr_dm_file_aes_gcm_decrypt(ct, sizeof(ct), k_key, k_nonce16, 8, pt, &pt_len));
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/gnostr/dm-files/qh4j/write-hex", test_write_hex);
  g_test_add_func("/gnostr/dm-files/qh4j/read-hex-16-byte-nonce-decrypts", test_read_hex_16_byte_nonce_decrypts);
  g_test_add_func("/gnostr/dm-files/qh4j/read-hex-12-byte-nonce-roundtrip", test_read_hex_12_byte_nonce_roundtrip);
  g_test_add_func("/gnostr/dm-files/qh4j/read-legacy-base64", test_read_legacy_base64);
  g_test_add_func("/gnostr/dm-files/qh4j/reject-malformed", test_reject_malformed);
  return g_test_run();
}
