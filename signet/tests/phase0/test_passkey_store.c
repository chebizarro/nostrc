/* SPDX-License-Identifier: MIT */
/*
 * Phase 0 spike: storage-fit + portability (sync) probe.
 *
 * Proves two things the plan hinges on:
 *  1. N passkeys per agent — a dedicated table with no UNIQUE(agent_pubkey),
 *     unlike signet's existing `secrets` table (store.c:141).
 *  2. Portability — a credential's private key is wrapped under a fleet sync
 *     key (PSK) with libsodium, moved to a *second* store ("instance B"), then
 *     unwrapped, re-imported, and used to produce a signature that verifies.
 */

#include "signet/fido_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>
#include <sodium.h>

#define OK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); return 1; } } while (0)

static const char *SCHEMA =
    "CREATE TABLE passkey_credentials ("
    "  credential_id BLOB PRIMARY KEY,"
    "  agent_id TEXT NOT NULL,"
    "  rp_id TEXT NOT NULL,"
    "  pub_x BLOB NOT NULL,"
    "  pub_y BLOB NOT NULL,"
    "  sign_count INTEGER NOT NULL DEFAULT 0,"
    "  aaguid BLOB,"
    "  discoverable INTEGER NOT NULL DEFAULT 1,"
    "  payload BLOB NOT NULL,"   /* PSK-wrapped private key */
    "  nonce BLOB NOT NULL,"
    "  created_at INTEGER NOT NULL);"
    "CREATE INDEX idx_pk_agent_rp ON passkey_credentials(agent_id, rp_id);";

static int exec_sql(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "sql error: %s\n", err ? err : "?");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

/* Insert one credential; returns 0 on success. */
static int insert_cred(sqlite3 *db, const uint8_t *cid, size_t cid_len,
                       const char *agent, const char *rp,
                       const uint8_t x[32], const uint8_t y[32],
                       const uint8_t *payload, size_t payload_len,
                       const uint8_t *nonce, size_t nonce_len)
{
    const char *sql =
        "INSERT INTO passkey_credentials"
        "(credential_id,agent_id,rp_id,pub_x,pub_y,payload,nonce,created_at)"
        " VALUES(?,?,?,?,?,?,?,0);";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_blob(st, 1, cid, (int)cid_len, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, agent, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, rp, -1, SQLITE_STATIC);
    sqlite3_bind_blob(st, 4, x, 32, SQLITE_STATIC);
    sqlite3_bind_blob(st, 5, y, 32, SQLITE_STATIC);
    sqlite3_bind_blob(st, 6, payload, (int)payload_len, SQLITE_STATIC);
    sqlite3_bind_blob(st, 7, nonce, (int)nonce_len, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int main(void)
{
    OK(sodium_init() >= 0, "sodium_init");

    /* Fleet sync key (PSK) — provisioned to every signet instance in the fleet. */
    uint8_t psk[crypto_secretbox_KEYBYTES];
    randombytes_buf(psk, sizeof(psk));

    /* ---- instance A: create 3 passkeys for the SAME agent ---- */
    sqlite3 *A = NULL;
    OK(sqlite3_open(":memory:", &A) == SQLITE_OK, "open A");
    OK(exec_sql(A, SCHEMA) == 0, "schema A");

    const char *agent = "agent-alpha";
    const char *rps[3] = { "github.com", "example.com", "google.com" };

    /* Keep the first credential's material to round-trip later. */
    uint8_t keep_x[32], keep_y[32];
    uint8_t keep_cid[16];
    uint8_t keep_wrapped[4096]; size_t keep_wrapped_len = 0;
    uint8_t keep_nonce[crypto_secretbox_NONCEBYTES];

    for (int i = 0; i < 3; i++) {
        signet_fido_key *k = signet_fido_key_generate();
        OK(k, "keygen");
        uint8_t x[32], y[32];
        OK(signet_fido_key_public_xy(k, x, y) == 0, "xy");

        uint8_t *der = NULL; size_t der_len = 0;
        OK(signet_fido_key_export_private(k, &der, &der_len) == 0, "export");

        /* wrap private key under the PSK */
        uint8_t nonce[crypto_secretbox_NONCEBYTES];
        randombytes_buf(nonce, sizeof(nonce));
        size_t ct_len = der_len + crypto_secretbox_MACBYTES;
        uint8_t *ct = malloc(ct_len);
        OK(ct, "alloc ct");
        OK(crypto_secretbox_easy(ct, der, der_len, nonce, psk) == 0, "wrap");

        uint8_t cid[16];
        randombytes_buf(cid, sizeof(cid));

        OK(insert_cred(A, cid, sizeof(cid), agent, rps[i], x, y, ct, ct_len,
                       nonce, sizeof(nonce)) == 0, "insert (N-per-agent)");

        if (i == 0) {
            memcpy(keep_x, x, 32); memcpy(keep_y, y, 32);
            memcpy(keep_cid, cid, 16);
            memcpy(keep_wrapped, ct, ct_len); keep_wrapped_len = ct_len;
            memcpy(keep_nonce, nonce, sizeof(nonce));
        }
        free(der); free(ct);
        signet_fido_key_free(k);
    }

    /* All three rows exist for one agent — no UNIQUE(agent_pubkey) conflict. */
    sqlite3_stmt *cnt = NULL;
    OK(sqlite3_prepare_v2(A, "SELECT COUNT(*) FROM passkey_credentials WHERE agent_id=?;",
                          -1, &cnt, NULL) == SQLITE_OK, "prep count");
    sqlite3_bind_text(cnt, 1, agent, -1, SQLITE_STATIC);
    OK(sqlite3_step(cnt) == SQLITE_ROW, "step count");
    OK(sqlite3_column_int(cnt, 0) == 3, "3 passkeys for one agent");
    sqlite3_finalize(cnt);

    /* ---- instance B: import the exported credential and use it ---- */
    /* Build an export container (subset) and move it. Instance B shares the PSK. */
    sqlite3 *B = NULL;
    OK(sqlite3_open(":memory:", &B) == SQLITE_OK, "open B");
    OK(exec_sql(B, SCHEMA) == 0, "schema B");
    OK(insert_cred(B, keep_cid, sizeof(keep_cid), agent, rps[0], keep_x, keep_y,
                   keep_wrapped, keep_wrapped_len, keep_nonce, sizeof(keep_nonce)) == 0,
       "import into B");

    /* Read it back on B, unwrap with the PSK, re-import, sign, verify. */
    sqlite3_stmt *sel = NULL;
    OK(sqlite3_prepare_v2(B,
        "SELECT pub_x,pub_y,payload,nonce FROM passkey_credentials WHERE credential_id=?;",
        -1, &sel, NULL) == SQLITE_OK, "prep select B");
    sqlite3_bind_blob(sel, 1, keep_cid, sizeof(keep_cid), SQLITE_STATIC);
    OK(sqlite3_step(sel) == SQLITE_ROW, "row on B");

    const uint8_t *bx = sqlite3_column_blob(sel, 0);
    const uint8_t *by = sqlite3_column_blob(sel, 1);
    const uint8_t *ct = sqlite3_column_blob(sel, 2);
    int ct_len = sqlite3_column_bytes(sel, 2);
    const uint8_t *nonce = sqlite3_column_blob(sel, 3);

    uint8_t bx2[32], by2[32];
    memcpy(bx2, bx, 32); memcpy(by2, by, 32);

    size_t pt_len = (size_t)ct_len - crypto_secretbox_MACBYTES;
    uint8_t *der = malloc(pt_len);
    OK(der, "alloc der");
    OK(crypto_secretbox_open_easy(der, ct, (size_t)ct_len, nonce, psk) == 0, "unwrap on B");

    signet_fido_key *k = signet_fido_key_import_private(der, pt_len);
    OK(k, "import on B");

    const uint8_t msg[] = "assertion made on instance B";
    uint8_t *sig = NULL; size_t sig_len = 0;
    OK(signet_fido_key_sign(k, msg, sizeof(msg) - 1, &sig, &sig_len) == 0, "sign on B");
    OK(signet_fido_verify_p256(bx2, by2, msg, sizeof(msg) - 1, sig, sig_len) == 1,
       "instance B assertion verifies against stored public key");

    /* Wrong PSK must fail to unwrap. */
    uint8_t wrong_psk[crypto_secretbox_KEYBYTES];
    randombytes_buf(wrong_psk, sizeof(wrong_psk));
    uint8_t *der2 = malloc(pt_len);
    OK(crypto_secretbox_open_easy(der2, ct, (size_t)ct_len, nonce, wrong_psk) != 0,
       "wrong PSK rejected");

    free(der); free(der2); free(sig);
    signet_fido_key_free(k);
    sqlite3_finalize(sel);
    sqlite3_close(A);
    sqlite3_close(B);

    printf("PASS test_passkey_store (N-per-agent storage; PSK-wrap export A->B; unwrap+sign+verify; wrong-PSK rejected)\n");
    return 0;
}
