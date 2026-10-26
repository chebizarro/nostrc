#define _GNU_SOURCE
#include "nostrdb.h"
#include "lmdb.h"
#include "cursor.h"
#include <ccan/crypto/sha256/sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); \
} } while (0)

static void remove_db(const char *dir)
{
    char path[256];
    snprintf(path, sizeof path, "%s/data.mdb", dir);
    CHECK(unlink(path) == 0);
    snprintf(path, sizeof path, "%s/lock.mdb", dir);
    CHECK(unlink(path) == 0);
    CHECK(rmdir(dir) == 0);
}

static void empty_values(void)
{
    char dir[] = "/tmp/nostrc-lmdb-ub-XXXXXX";
    MDB_env *env;
    MDB_txn *txn;
    MDB_dbi db;
    MDB_val key = {3, "key"}, empty = {0, NULL}, got;
    CHECK(mkdtemp(dir));
    CHECK(mdb_env_create(&env) == 0);
    CHECK(mdb_env_open(env, dir, 0, 0600) == 0);
    CHECK(mdb_txn_begin(env, NULL, 0, &txn) == 0);
    CHECK(mdb_dbi_open(txn, NULL, 0, &db) == 0);
    CHECK(mdb_put(txn, db, &key, &empty, 0) == 0); /* new leaf */
    CHECK(mdb_put(txn, db, &key, &empty, 0) == 0); /* same-size overwrite */
    CHECK(mdb_get(txn, db, &key, &got) == 0 && got.mv_size == 0);
    MDB_val full = {7, "payload"};
    CHECK(mdb_put(txn, db, &key, &full, 0) == 0);
    CHECK(mdb_put(txn, db, &key, &empty, 0) == 0); /* shrink to zero */
    CHECK(mdb_txn_commit(txn) == 0);
    CHECK(mdb_txn_begin(env, NULL, MDB_RDONLY, &txn) == 0);
    CHECK(mdb_get(txn, db, &key, &got) == 0 && got.mv_size == 0);
    mdb_txn_abort(txn);
    mdb_env_close(env);
    remove_db(dir);
}

static void tag_timestamps(void)
{
    char dir[] = "/tmp/nostrc-tags-ub-XXXXXX";
    struct ndb *ndb;
    struct ndb_config config;
    struct ndb_filter all;
    CHECK(mkdtemp(dir));
    ndb_default_config(&config);
    /* Structural index regression; real relayd tests verify signed ingestion. */
    ndb_config_set_flags(&config, NDB_FLAG_SKIP_NOTE_VERIFY);
    ndb_config_set_ingest_threads(&config, 1);
    ndb_config_set_mapsize(&config, 32 * 1024 * 1024);
    CHECK(ndb_init(&ndb, dir, &config));
    CHECK(ndb_filter_init(&all));
    CHECK(ndb_filter_start_field(&all, NDB_FILTER_KINDS));
    CHECK(ndb_filter_add_int_element(&all, 1));
    ndb_filter_end_field(&all);
    CHECK(ndb_filter_end(&all));
    uint64_t sub = ndb_subscribe(ndb, &all, 1);
    CHECK(sub);
    /* Tag byte + 1..8 value bytes put the trailing timestamp at every offset
     * modulo eight. Two timestamps per value force numeric comparisons. */
    for (int len = 1; len <= 8; ++len) {
        for (int ts = 1; ts <= 2; ++ts) {
            char json[1024];
            int n = snprintf(json, sizeof json,
                "[\"EVENT\",\"test\",{\"id\":\"%064x\",\"pubkey\":\"%064x\","
                "\"sig\":\"%0128x\",\"created_at\":%d,\"kind\":1,"
                "\"tags\":[[\"t\",\"%.*s\"]],\"content\":\"indexed word\"}]",
                len * 2 + ts, 1, 0, 1000 + ts, len, "abcdefgh");
            CHECK(n > 0 && n < (int)sizeof json);
            CHECK(ndb_process_event(ndb, json, n));
        }
    }
    int received = 0;
    while (received < 16) {
        uint64_t ids[16];
        int n = ndb_wait_for_notes(ndb, sub, ids, 16);
        CHECK(n > 0);
        received += n;
    }
    CHECK(ndb_unsubscribe(ndb, sub));
    ndb_filter_destroy(&all);
    ndb_destroy(ndb);
    /* Reopen to exercise the persisted tag index, not only an ingest buffer. */
    CHECK(ndb_init(&ndb, dir, &config));
    struct ndb_txn txn;
    CHECK(ndb_begin_query(ndb, &txn));
    for (int len = 1; len <= 8; ++len) {
        struct ndb_filter filter;
        struct ndb_query_result results[4];
        char value[9];
        int count = 0;
        snprintf(value, sizeof value, "%.*s", len, "abcdefgh");
        CHECK(ndb_filter_init(&filter));
        CHECK(ndb_filter_start_tag_field(&filter, 't'));
        CHECK(ndb_filter_add_str_element(&filter, value));
        ndb_filter_end_field(&filter);
        CHECK(ndb_filter_end(&filter));
        CHECK(ndb_query(&txn, &filter, 1, results, 4, &count));
        CHECK(count == 2);
        CHECK(ndb_note_created_at(results[0].note) == 1002);
        CHECK(ndb_note_created_at(results[1].note) == 1001);
        ndb_filter_destroy(&filter);
    }
    ndb_end_query(&txn);
    ndb_destroy(ndb);
    remove_db(dir);
}

static void full_width_varints(void)
{
    const uint64_t values[] = {0, 127, 128, UINT64_C(1) << 63, UINT64_MAX};
    for (size_t i = 0; i < sizeof values / sizeof values[0]; ++i) {
        unsigned char bytes[10];
        struct cursor writer, reader;
        uint64_t decoded = 0;
        make_cursor(bytes, bytes + sizeof bytes, &writer);
        int len = cursor_push_varint(&writer, values[i]);
        CHECK(len > 0);
        make_cursor(bytes, bytes + len, &reader);
        CHECK(cursor_pull_varint(&reader, &decoded) == len);
        CHECK(decoded == values[i]);
    }
}

static void unaligned_sha256(void)
{
    static const unsigned char expected[32] = {
        0x68, 0x36, 0xcf, 0x13, 0xba, 0xc4, 0x00, 0xe9,
        0x10, 0x50, 0x71, 0xcd, 0x6a, 0xf4, 0x70, 0x84,
        0xdf, 0xac, 0xad, 0x4e, 0x5e, 0x30, 0x2c, 0x94,
        0xbf, 0xed, 0x24, 0xe0, 0x13, 0xaf, 0xb7, 0x3e
    };
    unsigned char input[128 + 8];
    memset(input, 'a', sizeof input);
    for (size_t offset = 0; offset < 8; ++offset) {
        struct sha256 digest;
        sha256(&digest, input + offset, 128);
        CHECK(memcmp(digest.u.u8, expected, sizeof expected) == 0);
    }
}

static void unaligned_json_tokens(void)
{
    static const unsigned char expected_id[32] = {
        0xbc, 0x22, 0xfd, 0xf4, 0x0d, 0x80, 0xeb, 0xa2,
        0xb2, 0x43, 0xab, 0x3a, 0xb2, 0x1e, 0x9e, 0xe9,
        0xe5, 0x65, 0x37, 0x71, 0x38, 0xd0, 0x18, 0x5e,
        0x59, 0xfd, 0xb6, 0xcd, 0x34, 0x77, 0xd2, 0x15
    };
    unsigned char scratch[8192 + 8];
    char json[1024];
    int len = snprintf(json, sizeof json,
        "{\"id\":\"%064x\",\"pubkey\":\"%064x\",\"sig\":\"%0128x\","
        "\"created_at\":1000,\"kind\":1,\"tags\":[[\"t\",\"tag\"]],\"content\":\"word\"}",
        1, 1, 0);
    for (size_t offset = 0; offset < 8; ++offset) {
        struct ndb_note *note;
        CHECK(ndb_note_from_json(json, len, &note, scratch + offset, 8191));
        CHECK(ndb_note_created_at(note) == 1000);
        CHECK(ndb_note_kind(note) == 1);
        unsigned char commitment[1024], id[32 + 8];
        CHECK(ndb_calculate_id(note, commitment, sizeof commitment, id + offset));
        CHECK(memcmp(id + offset, expected_id, sizeof expected_id) == 0);
    }
}

int main(void)
{
    unaligned_json_tokens();
    unaligned_sha256();
    full_width_varints();
    empty_values();
    tag_timestamps();
    puts("PASS unaligned SHA256, full-width varints, empty values and tag timestamps");
    return 0;
}
