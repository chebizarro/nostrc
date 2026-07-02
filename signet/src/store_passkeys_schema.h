/* SPDX-License-Identifier: MIT */
#ifndef SIGNET_STORE_PASSKEYS_SCHEMA_H
#define SIGNET_STORE_PASSKEYS_SCHEMA_H

/*
 * Phase 1 passkey vault schema.
 *
 * Private key material is not stored in any lookup column. The payload column is
 * a versioned Signet passkey payload wrapped with the fleet PSK using
 * crypto_secretbox_easy; nonce is the corresponding XSalsa20-Poly1305 nonce.
 */
#define SIGNET_PASSKEY_CREDENTIALS_SCHEMA_SQL \
  "CREATE TABLE IF NOT EXISTS passkey_credentials (" \
  "  id INTEGER PRIMARY KEY AUTOINCREMENT," \
  "  credential_id BLOB NOT NULL," \
  "  agent_id TEXT NOT NULL," \
  "  rp_id TEXT NOT NULL," \
  "  rp_id_hash BLOB NOT NULL CHECK(length(rp_id_hash) = 32)," \
  "  user_handle BLOB NOT NULL," \
  "  sign_count INTEGER NOT NULL DEFAULT 0 CHECK(sign_count = 0)," \
  "  aaguid BLOB NOT NULL CHECK(length(aaguid) = 16)," \
  "  discoverable INTEGER NOT NULL DEFAULT 1," \
  "  payload BLOB NOT NULL," \
  "  nonce BLOB NOT NULL," \
  "  created_at INTEGER NOT NULL," \
  "  updated_at INTEGER NOT NULL" \
  ");" \
  "CREATE INDEX IF NOT EXISTS idx_passkey_credentials_agent_rp " \
  "ON passkey_credentials(agent_id, rp_id);" \
  "CREATE UNIQUE INDEX IF NOT EXISTS idx_passkey_credentials_credential_id " \
  "ON passkey_credentials(credential_id);"

#endif /* SIGNET_STORE_PASSKEYS_SCHEMA_H */
