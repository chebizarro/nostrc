/* SPDX-License-Identifier: MIT
 * SV-A2: vault_file_export_nip94 against a bounded local Blossom stub. The
 * stub answers BUD-02 PUT /upload with a descriptor and BUD-01 GET /<sha256>
 * from memory; no real server or relay is contacted. */
#include "nostrc-test-vault.h"
#include <vault/tree.h>
#include <nostr/nip44/kdf.h>
#include <openssl/evp.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>

typedef enum { STUB_OK, STUB_WRONG_HASH, STUB_FAIL_500, STUB_UNAUTHORIZED } stub_mode;
typedef struct {
    int listen_fd, port;
    pthread_t thread;
    atomic_int stop, mode, puts, authed_puts;
    uint8_t *blob;
    size_t blob_len;
    char blob_hash[65];
    pthread_mutex_t lock;
} stub;

/* Case-insensitive header lookup within the received headers (POSIX-only). */
static const char *header_value(const char *buf, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = buf; (p = strstr(p, "\r\n")); p += 2)
        if (!strncasecmp(p + 2, name, n))
            return p + 2 + n;
    return NULL;
}

static size_t read_request(int fd, char *buf, size_t cap)
{
    size_t total = 0, body_len = 0, header_len = 0;
    while (total + 1 < cap) {
        ssize_t n = recv(fd, buf + total, cap - 1 - total, 0);
        if (n <= 0)
            break;
        total += (size_t)n;
        buf[total] = 0;
        char *end = strstr(buf, "\r\n\r\n");
        if (!end)
            continue;
        header_len = (size_t)(end - buf) + 4;
        const char *cl = header_value(buf, "content-length:");
        body_len = cl ? strtoul(cl, NULL, 10) : 0;
        if (total - header_len >= body_len)
            break;
    }
    return header_len && total - header_len >= body_len ? header_len : 0;
}

static void send_all(int fd, const char *data, size_t len)
{
    while (len) {
        ssize_t n = send(fd, data, len, 0);
        if (n <= 0)
            return;
        data += n;
        len -= (size_t)n;
    }
}

static void *stub_thread(void *arg)
{
    stub *s = arg;
    signal(SIGPIPE, SIG_IGN);
    static char buf[256 * 1024];
    while (!atomic_load(&s->stop)) {
        int fd = accept(s->listen_fd, NULL, NULL);
        if (fd < 0)
            continue;
        size_t header_len = read_request(fd, buf, sizeof buf);
        char method[8] = {0}, path[160] = {0};
        if (header_len)
            sscanf(buf, "%7s %159s", method, path);
        char response[1024];
        int n = 0;
        if (!strcmp(method, "PUT") && !strcmp(path, "/upload")) {
            atomic_fetch_add(&s->puts, 1);
            if (strstr(buf, "\r\nAuthorization: Nostr "))
                atomic_fetch_add(&s->authed_puts, 1);
            const char *cl = header_value(buf, "content-length:");
            size_t len = cl ? strtoul(cl, NULL, 10) : 0;
            int mode = atomic_load(&s->mode);
            if (mode == STUB_FAIL_500 || mode == STUB_UNAUTHORIZED) {
                n = snprintf(response, sizeof response,
                             "HTTP/1.1 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                             mode == STUB_FAIL_500 ? "500 Internal Server Error"
                                                   : "401 Unauthorized");
            } else {
                uint8_t hash[32];
                char hex[65];
                assert(porthome_sha256((uint8_t *)buf + header_len, len, hash) == 0);
                porthome_hex64(hash, hex);
                pthread_mutex_lock(&s->lock);
                free(s->blob);
                s->blob = malloc(len ? len : 1);
                assert(s->blob);
                memcpy(s->blob, buf + header_len, len);
                s->blob_len = len;
                memcpy(s->blob_hash, hex, 65);
                pthread_mutex_unlock(&s->lock);
                if (mode == STUB_WRONG_HASH)
                    hex[0] = hex[0] == '0' ? '1' : '0';
                char body[512];
                int bn = snprintf(body, sizeof body,
                                  "{\"sha256\":\"%s\",\"size\":%zu,\"url\":\"http://127.0.0.1:%d/"
                                  "%s\",\"type\":\"application/octet-stream\",\"uploaded\":1}",
                                  hex, len, s->port, hex);
                n = snprintf(response, sizeof response,
                             "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                             "%d\r\nConnection: close\r\n\r\n%s",
                             bn, body);
            }
            send_all(fd, response, (size_t)n);
        } else if (!strcmp(method, "GET") && strlen(path) == 65) {
            pthread_mutex_lock(&s->lock);
            int found = s->blob && !strcmp(path + 1, s->blob_hash);
            if (found) {
                n = snprintf(response, sizeof response,
                             "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                             "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                             s->blob_len);
                send_all(fd, response, (size_t)n);
                send_all(fd, (const char *)s->blob, s->blob_len);
            }
            pthread_mutex_unlock(&s->lock);
            if (!found) {
                n = snprintf(response, sizeof response,
                             "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                send_all(fd, response, (size_t)n);
            }
        } else {
            n = snprintf(response, sizeof response,
                         "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            send_all(fd, response, (size_t)n);
        }
        close(fd);
    }
    return NULL;
}

static void stub_start(stub *s)
{
    memset(s, 0, sizeof *s);
    pthread_mutex_init(&s->lock, NULL);
    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(s->listen_fd >= 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    assert(inet_pton(AF_INET, "127.0.0.1", &a.sin_addr) == 1);
    assert(bind(s->listen_fd, (struct sockaddr *)&a, sizeof a) == 0);
    socklen_t size = sizeof a;
    assert(getsockname(s->listen_fd, (struct sockaddr *)&a, &size) == 0);
    s->port = ntohs(a.sin_port);
    assert(listen(s->listen_fd, 8) == 0);
    struct timeval tv = {.tv_usec = 100 * 1000};
    setsockopt(s->listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    assert(pthread_create(&s->thread, NULL, stub_thread, s) == 0);
}

static void stub_stop(stub *s)
{
    atomic_store(&s->stop, 1);
    shutdown(s->listen_fd, SHUT_RDWR);
    close(s->listen_fd);
    pthread_join(s->thread, NULL);
    free(s->blob);
    pthread_mutex_destroy(&s->lock);
}

/* Signer reusing libnostr, as libblossom's own tests do. */
static const char *SIGNER_SK = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static blossom_error_t sign_event(const char *json, char **out, void *user)
{
    (void)user;
    NostrEvent *ev = nostr_event_new();
    assert(ev);
    if (nostr_event_deserialize_compact(ev, json, NULL) != 1 || nostr_event_sign(ev, SIGNER_SK)) {
        nostr_event_free(ev);
        return BLOSSOM_ERR_NOSTR;
    }
    *out = nostr_event_serialize_compact(ev);
    nostr_event_free(ev);
    return *out ? BLOSSOM_OK : BLOSSOM_ERR_NOMEM;
}

static blossom_client_t *client_for(const stub *s, char *pubkey, char endpoint[64])
{
    snprintf(endpoint, 64, "http://127.0.0.1:%d", s->port);
    /* Seed the capability cache: no probe traffic, raw wire bytes, reachable. */
    blossom_server_capabilities_t caps;
    blossom_server_capabilities_init(&caps);
    caps.reachable = true;
    caps.raw_random_ok = BLOSSOM_CAP_YES;
    caps.private_server = BLOSSOM_CAP_YES;
    caps.last_probe_ts = (int64_t)time(NULL);
    blossom_capability_cache_set(endpoint, &caps);
    blossom_signer_t signer = {.pubkey = pubkey, .sign = sign_event};
    blossom_client_opts_t opts = {.endpoint = endpoint, .allow_insecure = true, .timeout_seconds = 5};
    blossom_client_t *client = NULL;
    assert(blossom_client_new(&opts, &signer, &client) == BLOSSOM_OK);
    return client;
}

static void unhex(const char *hex, uint8_t *out, size_t len)
{
    assert(strlen(hex) == 2 * len);
    for (size_t i = 0; i < len; ++i) {
        unsigned v;
        assert(sscanf(hex + 2 * i, "%2x", &v) == 1);
        out[i] = (uint8_t)v;
    }
}

/* Recipient side: decrypt with ONLY the returned tags (AES-256-GCM, ct||tag). */
static int decrypt_with_tags(const vault_nip94_tags *t, const uint8_t *ct, size_t ct_len,
                             uint8_t **out, size_t *out_len)
{
    uint8_t key[32], nonce[12];
    unhex(t->decryption_key, key, 32);
    unhex(t->decryption_nonce, nonce, 12);
    assert(!strcmp(t->encryption_algorithm, "aes-gcm") && ct_len >= 16);
    size_t pt_len = ct_len - 16;
    uint8_t *pt = malloc(pt_len ? pt_len : 1);
    assert(pt);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int outl = 0, finl = 0;
    int ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) == 1 &&
             EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, NULL) == 1 &&
             EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) == 1 &&
             EVP_DecryptUpdate(ctx, pt, &outl, ct, (int)pt_len) == 1 &&
             EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void *)(ct + pt_len)) == 1 &&
             EVP_DecryptFinal_ex(ctx, pt + outl, &finl) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        free(pt);
        return -1;
    }
    *out = pt;
    *out_len = (size_t)outl + (size_t)finl;
    return 0;
}

/* Fetch by x through the real libblossom GET (hash-verified), decrypt with the
 * tags only, and compare with the expected plaintext and ox. */
static void round_trip(blossom_client_t *client, const vault_nip94_tags *t, const uint8_t *expect,
                       size_t expect_len)
{
    uint8_t *ct = NULL, *pt = NULL, hash[32];
    size_t ct_len = 0, pt_len = 0;
    char hex[65];
    assert(blossom_client_get(client, t->x, &ct, &ct_len) == BLOSSOM_OK);
    assert(ct_len == t->size && t->size == expect_len + 16);
    assert(decrypt_with_tags(t, ct, ct_len, &pt, &pt_len) == 0);
    assert(pt_len == expect_len && (!expect_len || !memcmp(pt, expect, expect_len)));
    assert(porthome_sha256(pt, pt_len, hash) == 0);
    porthome_hex64(hash, hex);
    assert(!strcmp(hex, t->ox));
    assert(t->url && strstr(t->url, t->x));
    /* Tampered ciphertext must not decrypt. */
    ct[0] ^= 1;
    uint8_t *bad = NULL;
    size_t bad_len = 0;
    assert(decrypt_with_tags(t, ct, ct_len, &bad, &bad_len) < 0 && !bad);
    free(ct);
    free(pt);
}

/* Derived chunk key as libporthome computes it for a sealed v2 chunk:
 * HKDF-SHA256(salt "porthome/v2/chunk", ikm node_key, info = wire nonce). */
static void derived_chunk_key(const uint8_t node_key[32], const uint8_t *sealed, uint8_t out[32])
{
    static const char salt[] = "porthome/v2/chunk";
    uint8_t prk[32];
    assert(nostr_nip44_hkdf_extract((const uint8_t *)salt, sizeof salt - 1, node_key, 32, prk) == 0);
    assert(nostr_nip44_hkdf_expand(prk, sealed + 1, 12, out, 32) == 0);
}

static void assert_key_independent(const world *w, const porthome_ring_t *ring,
                                   const porthome_node_entry *file, const vault_nip94_tags *t)
{
    char hex[65];
    uint8_t derived[32];
    for (size_t i = 0; i < ring->count; ++i) {
        porthome_hex64(ring->entries[i].node_key, hex);
        assert(strcmp(hex, t->decryption_key));
        for (size_t c = 0; c < file->chunks_len; ++c)
            for (size_t b = 0; b < w->stored; ++b)
                if (!memcmp(w->store[b].hash, file->chunks[c].sha256, 32)) {
                    assert(w->store[b].bytes[0] == PORTHOME_WIRE_VERSION_V2);
                    derived_chunk_key(ring->entries[i].node_key, w->store[b].bytes, derived);
                    porthome_hex64(derived, hex);
                    assert(strcmp(hex, t->decryption_key));
                }
    }
    /* Neither the hex strings nor the raw bytes ever contain ring material. */
    for (size_t i = 0; i < ring->count; ++i) {
        uint8_t key[32];
        unhex(t->decryption_key, key, 32);
        assert(memcmp(key, ring->entries[i].node_key, 32));
    }
}

int main(void)
{
    world w;
    world_init(&w);
    stub s;
    stub_start(&s);
    char *pubkey = nostr_key_get_public(SIGNER_SK), endpoint[64];
    assert(pubkey);
    blossom_client_t *client = client_for(&s, pubkey, endpoint);
    porthome_ring_t ring = vault_ring_view(&w.ring);
    vault_blob_io io = {&w, read_blob, put_blob};

    /* Single-chunk file with a MIME type: round trip and key independence. */
    vault_nip94_tags *t = NULL;
    assert(vault_file_export_nip94(&w.entries[0], &ring, &io, client, "text/plain", &t) == 0 && t);
    assert(!strcmp(t->encryption_algorithm, "aes-gcm") && !strcmp(t->file_type, "text/plain"));
    assert(strlen(t->decryption_key) == 64 && strlen(t->decryption_nonce) == 24 &&
           strlen(t->x) == 64 && strlen(t->ox) == 64 && strcmp(t->x, t->ox));
    const uint8_t hello[] = {'h', 'e', 'l', 'l', 'o', 0, 0};
    round_trip(client, t, hello, sizeof hello);
    assert_key_independent(&w, &ring, &w.entries[0], t);
    assert(atomic_load(&s.puts) == 1 && atomic_load(&s.authed_puts) == 1);
    /* A second export uses a fresh key, nonce and therefore address. */
    vault_nip94_tags *again = NULL;
    assert(vault_file_export_nip94(&w.entries[0], &ring, &io, client, NULL, &again) == 0);
    assert(strcmp(again->decryption_key, t->decryption_key) &&
           strcmp(again->decryption_nonce, t->decryption_nonce) && strcmp(again->x, t->x) &&
           !strcmp(again->ox, t->ox) && !again->file_type);
    vault_nip94_tags_free(again);
    vault_nip94_tags_free(t);

    /* Multi-chunk file across two epochs, read through exact per-chunk keys. */
    uint64_t epoch2 = 0;
    assert(vault_ring_append_random(&w.ring, &epoch2) == 0 && epoch2 == 2);
    ring = vault_ring_view(&w.ring);
    uint8_t part1[300], part2[70001], part3[5];
    for (size_t i = 0; i < sizeof part1; ++i)
        part1[i] = (uint8_t)(i * 7);
    for (size_t i = 0; i < sizeof part2; ++i)
        part2[i] = (uint8_t)(i ^ (i >> 8));
    memcpy(part3, "tail!", 5);
    porthome_node_chunk chunks[3] = {chunk(&w, ring.entries[0].node_key, 1, part1, sizeof part1),
                                     chunk(&w, ring.entries[1].node_key, 2, part2, sizeof part2),
                                     chunk(&w, ring.entries[0].node_key, 1, part3, sizeof part3)};
    porthome_node_entry multi = w.entries[0];
    multi.chunks = chunks;
    multi.chunks_len = 3;
    multi.size_plain = sizeof part1 + sizeof part2 + sizeof part3;
    uint8_t *joined = malloc(multi.size_plain);
    assert(joined);
    memcpy(joined, part1, sizeof part1);
    memcpy(joined + sizeof part1, part2, sizeof part2);
    memcpy(joined + sizeof part1 + sizeof part2, part3, sizeof part3);
    unsigned reads = w.reads;
    assert(vault_file_export_nip94(&multi, &ring, &io, client, "application/pdf", &t) == 0);
    assert(w.reads == reads + 3);
    round_trip(client, t, joined, multi.size_plain);
    assert_key_independent(&w, &ring, &multi, t);
    vault_nip94_tags_free(t);
    free(joined);
    /* Missing epoch fails closed before any I/O. */
    porthome_ring_t only_first = {ring.entries, 1};
    reads = w.reads;
    int put_count = atomic_load(&s.puts);
    assert(vault_file_export_nip94(&multi, &only_first, &io, client, NULL, &t) == VAULT_ERR_EPOCH);
    assert(!t && w.reads == reads && atomic_load(&s.puts) == put_count);

    /* Empty file: zero chunks, ciphertext is the 16-byte tag alone. */
    porthome_node_entry empty = w.entries[0];
    empty.chunks = NULL;
    empty.chunks_len = 0;
    empty.size_plain = 0;
    assert(vault_file_export_nip94(&empty, &ring, &io, client, "text/plain", &t) == 0);
    assert(t->size == 16);
    round_trip(client, t, NULL, 0);
    vault_nip94_tags_free(t);

    /* Oversize refusal before any read or upload. */
    porthome_node_entry huge = w.entries[0];
    huge.size_plain = (uint64_t)VAULT_NIP94_MAX_EXPORT_BYTES + 1;
    reads = w.reads;
    put_count = atomic_load(&s.puts);
    assert(vault_file_export_nip94(&huge, &ring, &io, client, NULL, &t) == PORTHOME_ERR_TOO_LARGE);
    assert(!t && w.reads == reads && atomic_load(&s.puts) == put_count);

    /* Argument gates: directories, bad MIME strings, inconsistent lengths. */
    porthome_node_entry dir = {.kind = PORTHOME_KIND_DIR};
    assert(vault_file_export_nip94(&dir, &ring, &io, client, NULL, &t) == VAULT_ERR_ARG);
    assert(vault_file_export_nip94(&w.entries[0], &ring, &io, client, "text/plain; x", &t) ==
           VAULT_ERR_ARG);
    porthome_node_entry short_file = w.entries[0];
    short_file.size_plain--;
    assert(vault_file_export_nip94(&short_file, &ring, &io, client, NULL, &t) == VAULT_ERR_ARG);
    assert(!t);

    /* Upload hash mismatch: the server's descriptor names another blob. */
    atomic_store(&s.mode, STUB_WRONG_HASH);
    assert(vault_file_export_nip94(&w.entries[0], &ring, &io, client, NULL, &t) ==
           PORTHOME_ERR_HASH_MISMATCH);
    assert(!t);
    /* Upload failures are classified, never reported as success. */
    atomic_store(&s.mode, STUB_FAIL_500);
    assert(vault_file_export_nip94(&w.entries[0], &ring, &io, client, NULL, &t) == VAULT_ERR_IO);
    assert(!t);
    atomic_store(&s.mode, STUB_UNAUTHORIZED);
    assert(vault_file_export_nip94(&w.entries[0], &ring, &io, client, NULL, &t) == VAULT_ERR_AUTH);
    assert(!t);
    /* A client without a signer cannot use the upload path at all. */
    blossom_client_opts_t anon_opts = {.endpoint = endpoint, .allow_insecure = true};
    blossom_client_t *anon = NULL;
    assert(blossom_client_new(&anon_opts, NULL, &anon) == BLOSSOM_OK);
    atomic_store(&s.mode, STUB_OK);
    put_count = atomic_load(&s.puts);
    assert(vault_file_export_nip94(&w.entries[0], &ring, &io, anon, NULL, &t) == VAULT_ERR_AUTH);
    assert(!t && atomic_load(&s.puts) == put_count);
    blossom_client_free(anon);
    /* Corrupted sealed chunk: the authenticated reader refuses, nothing uploads. */
    w.corrupt = 1;
    assert(vault_file_export_nip94(&w.entries[0], &ring, &io, client, NULL, &t) ==
           PORTHOME_ERR_HASH_MISMATCH);
    assert(!t && atomic_load(&s.puts) == put_count);
    w.corrupt = 0;

    blossom_client_free(client);
    free(pubkey);
    stub_stop(&s);
    world_dispose(&w);
    puts("OK NIP-94/kind-15 export: fresh AES-GCM key, tags-only decrypt, epochs, limits, "
         "classified upload failures (local Blossom stub)");
    return 0;
}
