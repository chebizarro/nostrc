/* nostrc-0d0d (privacy charter PD-6): no two libnostr relay connections are
 * linkable through TLS session resumption.
 *
 * libwebsockets (built with LWS_WITH_TLS_SESSIONS, as Homebrew's 4.5 is)
 * keeps a client session cache per vhost, keyed by host and port, and offers
 * a cached session on the next connection to that relay. libnostr uses one
 * process-wide lws context for every relay connection (connection.c), so a
 * relay could link connections made for different accounts or purposes, even
 * across IP address changes. libnostr now creates that context with
 * LWS_SERVER_OPTION_DISABLE_TLS_SESSION_CACHE and SSL_OP_NO_TICKET.
 *
 * The TLS server here is OpenSSL's, a thread per connection, on 127.0.0.1
 * with a self-signed certificate made at start (trusted through
 * SSL_CERT_FILE, which OpenSSL's default verify paths read and lws loads for
 * its client context). Its ClientHello callback records whether each
 * ClientHello offers a session: a TLS 1.3 pre_shared_key (extension 41) or a
 * non-empty TLS 1.2 SessionTicket (35); SSL_session_reused() says whether the
 * handshake resumed one. It answers WebSocket upgrades (then reads until the
 * client leaves) and anything else with a small HTTP response.
 *  1. Control: a plain OpenSSL client that keeps its session MUST offer and
 *     resume it on its second connection; otherwise this harness could not
 *     see what it tests.
 *  2. Control: a libwebsockets client context of its own, made as libnostr's
 *     used to be (session cache on), MUST offer a session on its second
 *     connection when lws has TLS sessions: the leak this test guards.
 *  3. Two libnostr connections to the same relay: neither may offer or
 *     resume a session. Before the fix the second one resumed the first's.
 */
#include <libwebsockets.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "connection-private.h" /* nostr_connection_wait_handshake */
#include "nostr-connection.h"

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s (%s) at %s:%d\n", msg, #cond, \
                    __FILE__, __LINE__);                                     \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static double now_seconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

/* ---- a certificate for 127.0.0.1 ------------------------------------------------- */

static EVP_PKEY *g_key;
static X509 *g_cert;
static char *g_cert_pem;

static void make_certificate(void) {
    g_key = EVP_EC_gen("P-256");
    CHECK(g_key, "key generation");
    g_cert = X509_new();
    CHECK(g_cert, "X509_new");
    X509_set_version(g_cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(g_cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(g_cert), -3600);
    X509_gmtime_adj(X509_getm_notAfter(g_cert), 3600L * 24);
    X509_NAME *name = X509_get_subject_name(g_cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"127.0.0.1",
                               -1, -1, 0);
    X509_set_issuer_name(g_cert, name);
    X509_set_pubkey(g_cert, g_key);
    X509V3_CTX v3;
    X509V3_set_ctx_nodb(&v3);
    X509V3_set_ctx(&v3, g_cert, g_cert, NULL, NULL, 0);
    static const char *const extensions[][2] = {
        { "subjectAltName", "IP:127.0.0.1" },
        { "basicConstraints", "critical,CA:TRUE" },
        { "keyUsage", "critical,digitalSignature,keyCertSign" },
        { "extendedKeyUsage", "serverAuth" },
    };
    for (size_t i = 0; i < sizeof extensions / sizeof extensions[0]; i++) {
        X509_EXTENSION *ext = X509V3_EXT_conf(NULL, &v3, extensions[i][0], extensions[i][1]);
        CHECK(ext, "certificate extension");
        X509_add_ext(g_cert, ext, -1);
        X509_EXTENSION_free(ext);
    }
    CHECK(X509_sign(g_cert, g_key, EVP_sha256()) > 0, "certificate signature");
    BIO *bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, g_cert);
    char *data = NULL;
    long len = BIO_get_mem_data(bio, &data);
    g_cert_pem = malloc((size_t)len + 1);
    CHECK(g_cert_pem, "oom");
    memcpy(g_cert_pem, data, (size_t)len);
    g_cert_pem[len] = '\0';
    BIO_free(bio);
}

/* ---- the TLS server --------------------------------------------------------------- */

#define MAX_HELLOS 16

typedef struct {
    int psk;     /* pre_shared_key (41) */
    long ticket; /* SessionTicket (35) payload length; -1 without the extension */
    int resumed; /* SSL_session_reused() */
    int failed;  /* the handshake failed */
} Hello;

static SSL_CTX *g_srv_ctx;
static int g_listener = -1;
static int g_port;
static atomic_int g_stop;
static pthread_t g_acceptor;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Hello g_hellos[MAX_HELLOS]; /* guarded by g_lock, in handshake order */
static int g_nhellos;              /* guarded by g_lock */
static int g_worker_fds[MAX_HELLOS];
static pthread_t g_workers[MAX_HELLOS];
static int g_nworkers; /* acceptor thread only, read after it is joined */

typedef struct {
    Hello hello;
} Pending;

static int on_client_hello(SSL *ssl, int *alert, void *arg) {
    (void)alert;
    (void)arg;
    Pending *pending = SSL_get_app_data(ssl);
    const unsigned char *ext = NULL;
    size_t length = 0;
    if (SSL_client_hello_get0_ext(ssl, 41, &ext, &length))
        pending->hello.psk = 1;
    if (SSL_client_hello_get0_ext(ssl, 35, &ext, &length) && (long)length > pending->hello.ticket)
        pending->hello.ticket = (long)length;
    return SSL_CLIENT_HELLO_SUCCESS;
}

/* Reads a request head (to its blank line) into buffer; 0 at EOF. */
static int read_head(SSL *ssl, char *buffer, size_t size) {
    size_t used = 0;
    while (used + 1 < size) {
        int got = SSL_read(ssl, buffer + used, (int)(size - 1 - used));
        if (got <= 0) return 0;
        used += (size_t)got;
        buffer[used] = '\0';
        if (strstr(buffer, "\r\n\r\n")) return 1;
    }
    return 0;
}

static char *websocket_accept(const char *head) {
    const char *key = strstr(head, "Sec-WebSocket-Key:");
    if (!key) key = strstr(head, "sec-websocket-key:");
    if (!key) return NULL;
    key += strlen("Sec-WebSocket-Key:");
    while (*key == ' ') key++;
    char input[128];
    size_t klen = strcspn(key, "\r\n");
    if (klen > 64) return NULL;
    snprintf(input, sizeof input, "%.*s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", (int)klen, key);
    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char *)input, strlen(input), digest);
    char *out = malloc(64);
    CHECK(out, "oom");
    EVP_EncodeBlock((unsigned char *)out, digest, SHA_DIGEST_LENGTH);
    return out;
}

static void write_all(SSL *ssl, const char *text) {
    (void)SSL_write(ssl, text, (int)strlen(text));
}

static void *serve(void *arg) {
    int fd = (int)(intptr_t)arg;
    Pending pending = { .hello = { .ticket = -1 } };
    SSL *ssl = SSL_new(g_srv_ctx);
    SSL_set_app_data(ssl, &pending);
    SSL_set_fd(ssl, fd);
    int accepted = SSL_accept(ssl);
    pending.hello.failed = accepted != 1;
    pending.hello.resumed = accepted == 1 && SSL_session_reused(ssl);
    if (pending.hello.failed) {
        fprintf(stderr, "server handshake failed:\n");
        ERR_print_errors_fp(stderr);
    }
    pthread_mutex_lock(&g_lock);
    if (g_nhellos < MAX_HELLOS) g_hellos[g_nhellos++] = pending.hello;
    pthread_mutex_unlock(&g_lock);
    char head[8192];
    if (!pending.hello.failed && read_head(ssl, head, sizeof head)) {
        char *accept = websocket_accept(head);
        if (accept) {
            char reply[256];
            snprintf(reply, sizeof reply,
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Sec-WebSocket-Accept: %s\r\n\r\n",
                     accept);
            free(accept);
            write_all(ssl, reply);
            /* Kept open (frames ignored) until the client or the end closes it. */
            while (SSL_read(ssl, head, sizeof head) > 0)
                ;
        } else {
            write_all(ssl, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
            (void)SSL_shutdown(ssl);
        }
    }
    SSL_free(ssl);
    return NULL;
}

static void *accept_loop(void *arg) {
    (void)arg;
    while (!atomic_load(&g_stop)) {
        struct pollfd listener = { g_listener, POLLIN, 0 };
        if (poll(&listener, 1, 50) <= 0) continue;
        int fd = accept(g_listener, NULL, NULL);
        if (fd < 0) continue;
        if (g_nworkers >= MAX_HELLOS) {
            close(fd);
            continue;
        }
        g_worker_fds[g_nworkers] = fd;
        CHECK(pthread_create(&g_workers[g_nworkers], NULL, serve, (void *)(intptr_t)fd) == 0,
              "worker thread");
        g_nworkers++;
    }
    return NULL;
}

static void server_start(void) {
    g_srv_ctx = SSL_CTX_new(TLS_server_method());
    CHECK(g_srv_ctx, "server SSL_CTX");
    CHECK(SSL_CTX_use_certificate(g_srv_ctx, g_cert) == 1, "server certificate");
    CHECK(SSL_CTX_use_PrivateKey(g_srv_ctx, g_key) == 1, "server key");
    /* Session tickets are OpenSSL's server default (two per TLS 1.3 handshake). */
    SSL_CTX_set_client_hello_cb(g_srv_ctx, on_client_hello, NULL);
    g_listener = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(g_listener >= 0, "socket");
    struct sockaddr_in address = { 0 };
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(g_listener, (struct sockaddr *)&address, sizeof address) == 0, "bind");
    CHECK(listen(g_listener, 16) == 0, "listen");
    socklen_t length = sizeof address;
    CHECK(getsockname(g_listener, (struct sockaddr *)&address, &length) == 0, "getsockname");
    g_port = ntohs(address.sin_port);
    CHECK(pthread_create(&g_acceptor, NULL, accept_loop, NULL) == 0, "acceptor thread");
}

static void server_stop(void) {
    atomic_store(&g_stop, 1);
    pthread_join(g_acceptor, NULL);
    close(g_listener);
    /* Wakes every worker blocked in SSL_read, then waits for it. */
    for (int i = 0; i < g_nworkers; i++) shutdown(g_worker_fds[i], SHUT_RDWR);
    for (int i = 0; i < g_nworkers; i++) {
        pthread_join(g_workers[i], NULL);
        close(g_worker_fds[i]);
    }
    SSL_CTX_free(g_srv_ctx);
}

static int hello_count(void) {
    pthread_mutex_lock(&g_lock);
    int n = g_nhellos;
    pthread_mutex_unlock(&g_lock);
    return n;
}

static void wait_hellos(int count) {
    double deadline = now_seconds() + 20.0;
    while (hello_count() < count && now_seconds() < deadline) usleep(10000);
    CHECK(hello_count() >= count, "the server never saw the expected handshakes");
}

static Hello hello_at(int index) {
    pthread_mutex_lock(&g_lock);
    Hello hello = g_hellos[index];
    pthread_mutex_unlock(&g_lock);
    return hello;
}

static int offers(Hello hello) {
    return hello.psk || hello.ticket > 0 || hello.resumed;
}

static void report(const char *label, int index) {
    Hello hello = hello_at(index);
    CHECK(!hello.failed, "a handshake failed");
    printf("%-22s pre_shared_key=%-3s session_ticket=%-3ld resumed=%s%s\n", label,
           hello.psk ? "yes" : "no", hello.ticket, hello.resumed ? "yes" : "no",
           offers(hello) ? "  <- offers a session" : "");
    fflush(stdout);
}

/* ---- 1. a plain OpenSSL client that resumes --------------------------------------- */

static SSL_SESSION *openssl_fetch(SSL_CTX *ctx, SSL_SESSION *resume) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0, "socket");
    struct sockaddr_in address = { 0 };
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)g_port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(connect(fd, (struct sockaddr *)&address, sizeof address) == 0, "connect");
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    if (resume) SSL_set_session(ssl, resume);
    CHECK(SSL_connect(ssl) == 1, "control handshake");
    write_all(ssl, "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    char buffer[512];
    while (SSL_read(ssl, buffer, sizeof buffer) > 0) /* to EOF: reads the tickets */
        ;
    SSL_SESSION *session = SSL_get1_session(ssl);
    (void)SSL_shutdown(ssl); /* else SSL_free() marks the session not resumable */
    SSL_free(ssl);
    close(fd);
    return session;
}

/* ---- 2. a libwebsockets client context with the session cache on ------------------- */

static atomic_int g_lws_established;
static atomic_int g_lws_failed;

static int control_cb(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in,
                      size_t len) {
    (void)wsi;
    (void)user;
    (void)len;
    if (reason == LWS_CALLBACK_CLIENT_ESTABLISHED) atomic_fetch_add(&g_lws_established, 1);
    if (reason == LWS_CALLBACK_CLIENT_CONNECTION_ERROR) {
        fprintf(stderr, "control lws connection error: %s\n", in ? (const char *)in : "?");
        atomic_fetch_add(&g_lws_failed, 1);
    }
    return 0;
}

static struct lws_protocols g_control_protocols[] = {
    { "wss", control_cb, 0, 4096, 0, NULL, 0 },
    { NULL, NULL, 0, 0, 0, NULL, 0 }
};

static struct lws_context *g_control_ctx;
static atomic_int g_control_stop;

static void *control_service(void *arg) {
    (void)arg;
    while (!atomic_load(&g_control_stop)) lws_service(g_control_ctx, 50);
    return NULL;
}

static void control_connect(int established_before) {
    struct lws_client_connect_info ci;
    memset(&ci, 0, sizeof ci);
    ci.context = g_control_ctx;
    ci.address = "127.0.0.1";
    ci.port = g_port;
    ci.path = "/";
    ci.host = "127.0.0.1";
    ci.origin = "127.0.0.1";
    ci.ssl_connection = LCCSCF_USE_SSL;
    ci.protocol = "wss";
    CHECK(lws_client_connect_via_info(&ci), "control lws connect");
    lws_cancel_service(g_control_ctx);
    double deadline = now_seconds() + 20.0;
    while (atomic_load(&g_lws_established) <= established_before && !atomic_load(&g_lws_failed) &&
           now_seconds() < deadline)
        usleep(10000);
    CHECK(atomic_load(&g_lws_established) > established_before, "control lws never connected");
}

/* ---- the checks ------------------------------------------------------------------- */

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    unsetenv("NOSTR_TEST_MODE"); /* the real network path */
    lws_set_log_level(LLL_ERR, NULL);
    make_certificate();

    /* libnostr's context loads OpenSSL's default verify paths: trust the
     * certificate through SSL_CERT_FILE (read when the context is made). */
    char ca_path[] = "/tmp/nostrc-tls-resumption-XXXXXX";
    int ca_fd = mkstemp(ca_path);
    CHECK(ca_fd >= 0, "mkstemp");
    CHECK(write(ca_fd, g_cert_pem, strlen(g_cert_pem)) == (ssize_t)strlen(g_cert_pem), "write CA");
    close(ca_fd);
    setenv("SSL_CERT_FILE", ca_path, 1);

    server_start();

    /* 1. A plain OpenSSL client keeping its session resumes it. */
    SSL_CTX *client = SSL_CTX_new(TLS_client_method());
    CHECK(client, "client SSL_CTX");
    SSL_SESSION *first = openssl_fetch(client, NULL);
    CHECK(first && SSL_SESSION_is_resumable(first), "the server issued no resumable session");
    SSL_SESSION *second = openssl_fetch(client, first);
    SSL_SESSION_free(first);
    SSL_SESSION_free(second);
    SSL_CTX_free(client);
    wait_hellos(2);
    printf("-- control: a plain OpenSSL client, which must resume the second time\n");
    report("openssl 1", 0);
    report("openssl 2", 1);
    CHECK(!offers(hello_at(0)), "the first control connection had nothing to offer");
    CHECK(hello_at(1).psk && hello_at(1).resumed,
          "the control did not resume: this harness cannot see TLS session resumption");

    /* 2. libwebsockets with its client session cache, as libnostr's context
     *    was made before nostrc-0d0d. */
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof info);
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = g_control_protocols;
    info.gid = (gid_t)-1;
    info.uid = (uid_t)-1;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.client_ssl_ca_mem = g_cert_pem;
    info.client_ssl_ca_mem_len = (unsigned int)strlen(g_cert_pem);
    g_control_ctx = lws_create_context(&info);
    CHECK(g_control_ctx, "control lws context");
    pthread_t control_thread;
    CHECK(pthread_create(&control_thread, NULL, control_service, NULL) == 0, "control thread");
    control_connect(0);
    control_connect(1);
    wait_hellos(4);
    printf("-- control: libwebsockets with its session cache\n");
    report("lws cache on 1", 2);
    report("lws cache on 2", 3);
#if defined(LWS_WITH_TLS_SESSIONS)
    CHECK(offers(hello_at(3)),
          "libwebsockets (LWS_WITH_TLS_SESSIONS) did not offer its cached session: this "
          "harness cannot show what the fix prevents");
#else
    printf("(this libwebsockets has no TLS session cache; control 2 is informational)\n");
#endif

    /* 3. libnostr: two connections to the same relay. */
    char url[64];
    snprintf(url, sizeof url, "wss://127.0.0.1:%d/", g_port);
    NostrConnection *a = nostr_connection_new(url);
    CHECK(a, "libnostr connection 1");
    CHECK(nostr_connection_wait_handshake(a, 20000) == 1, "libnostr connection 1 handshake");
    NostrConnection *b = nostr_connection_new(url);
    CHECK(b, "libnostr connection 2");
    CHECK(nostr_connection_wait_handshake(b, 20000) == 1, "libnostr connection 2 handshake");
    wait_hellos(6);
    printf("-- libnostr: no connection may offer or resume a session\n");
    report("libnostr 1", 4);
    report("libnostr 2", 5);
    CHECK(hello_count() == 6, "unexpected extra handshakes");
    CHECK(!offers(hello_at(4)) && !offers(hello_at(5)),
          "a libnostr connection offered a TLS session: relays can link its connections");

    nostr_connection_close(a);
    nostr_connection_close(b);
    atomic_store(&g_control_stop, 1);
    lws_cancel_service(g_control_ctx);
    pthread_join(control_thread, NULL);
    lws_context_destroy(g_control_ctx);
    server_stop();
    unlink(ca_path);
    X509_free(g_cert);
    EVP_PKEY_free(g_key);
    free(g_cert_pem);
    printf("PASS: no libnostr TLS connection offered or resumed a session\n");
    return 0;
}
