/**
 * @file mock_relay_server.c
 * @brief Standalone mock relay WebSocket server implementation
 *
 * Uses libwebsockets to provide a WebSocket server that simulates a Nostr relay.
 */
#include "nostr/testing/mock_relay_server.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-envelope.h"
#include "channel.h"

#include <libwebsockets.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <nsync.h>
#include <jansson.h>
#include <time.h>

/* Maximum number of seeded/published events */
#define MAX_EVENTS 10000
/* Maximum message size */
#define MAX_MSG_SIZE (1024 * 1024)
/* Ring buffer size for pending write data */
#define RING_DEPTH 64

/* Per-connection state */
typedef struct MockConnection {
    struct lws *wsi;
    struct MockConnection *next;
    /* Pending write messages (ring buffer) */
    char *pending[RING_DEPTH];
    size_t pending_len[RING_DEPTH];
    int ring_head;
    int ring_tail;
    int ring_count;
    /* Active subscriptions - simplified: store sub_id only */
    char *subscriptions[16];
    int sub_count;
} MockConnection;

/* Internal server structure */
struct NostrMockRelayServer {
    /* Configuration (copied) */
    NostrMockRelayServerConfig config;
    char *bind_addr;
    char *cert_path;
    char *key_path;
    char *seed_file;
    char *relay_name;
    char *relay_desc;

    /* URL string cache */
    char url[256];
    uint16_t actual_port;

    /* libwebsockets context */
    struct lws_context *lws_ctx;

    /* Service thread */
    pthread_t service_thread;
    volatile int running;
    volatile int should_stop;

    /* Thread safety */
    nsync_mu mutex;
    nsync_cv cond_publish;

    /* Event stores - using JSON strings for simplicity */
    char *seeded_events[MAX_EVENTS];
    size_t seeded_count;

    char *published_events[MAX_EVENTS];
    size_t published_count;

    /* Connections list */
    MockConnection *connections;
    size_t conn_count;
    size_t conn_total;

    /* Statistics */
    size_t events_matched;
    size_t subs_received;
    size_t close_received;

    /* NIP-11 document */
    char *nip11_json;
};

/* Forward declarations */
static int mock_ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                            void *user, void *in, size_t len);
static int mock_http_callback(struct lws *wsi, enum lws_callback_reasons reason,
                              void *user, void *in, size_t len);
static void *service_thread_func(void *arg);
static void handle_req_envelope(NostrMockRelayServer *server, MockConnection *conn,
                                 const char *msg, size_t len);
static void handle_event_envelope(NostrMockRelayServer *server, MockConnection *conn,
                                   const char *msg, size_t len);
static void handle_close_envelope(NostrMockRelayServer *server, MockConnection *conn,
                                   const char *msg, size_t len);
static void send_to_connection(MockConnection *conn, const char *msg);
static char *build_default_nip11(NostrMockRelayServer *server);
static bool filter_matches_event_json(const char *filter_json, const char *event_json);

/* Protocol definitions */
static const struct lws_protocols protocols[] = {
    {
        .name = "http",
        .callback = mock_http_callback,
        .per_session_data_size = 0,
        .rx_buffer_size = 0,
    },
    {
        .name = "nostr",
        .callback = mock_ws_callback,
        .per_session_data_size = sizeof(MockConnection*),
        .rx_buffer_size = MAX_MSG_SIZE,
    },
    LWS_PROTOCOL_LIST_TERM
};

/* === Public API Implementation === */

NostrMockRelayServerConfig nostr_mock_server_config_default(void) {
    NostrMockRelayServerConfig cfg = {0};
    cfg.port = 0;  /* auto-assign */
    cfg.bind_addr = NULL;  /* defaults to 127.0.0.1 */
    cfg.use_tls = false;
    cfg.cert_path = NULL;
    cfg.key_path = NULL;
    cfg.seed_file = NULL;
    cfg.relay_name = NULL;  /* defaults to "MockRelay" */
    cfg.relay_desc = NULL;
    cfg.auto_eose = true;
    cfg.validate_signatures = false;
    cfg.response_delay_ms = 0;
    cfg.max_events_per_req = -1;  /* unlimited */
    return cfg;
}

NostrMockRelayServer *nostr_mock_server_new(const NostrMockRelayServerConfig *config) {
    NostrMockRelayServer *server = calloc(1, sizeof(NostrMockRelayServer));
    if (!server) return NULL;

    /* Apply config or defaults */
    NostrMockRelayServerConfig cfg = config ? *config : nostr_mock_server_config_default();
    server->config = cfg;

    /* Copy strings that need to persist */
    server->bind_addr = cfg.bind_addr ? strdup(cfg.bind_addr) : strdup("127.0.0.1");
    server->cert_path = cfg.cert_path ? strdup(cfg.cert_path) : NULL;
    server->key_path = cfg.key_path ? strdup(cfg.key_path) : NULL;
    server->seed_file = cfg.seed_file ? strdup(cfg.seed_file) : NULL;
    server->relay_name = cfg.relay_name ? strdup(cfg.relay_name) : strdup("MockRelay");
    server->relay_desc = cfg.relay_desc ? strdup(cfg.relay_desc) : strdup("Mock relay for testing");

    /* Initialize synchronization */
    nsync_mu_init(&server->mutex);
    nsync_cv_init(&server->cond_publish);

    /* Load seed file if specified */
    if (server->seed_file) {
        int loaded = nostr_mock_server_seed_from_jsonl(server, server->seed_file);
        if (loaded < 0) {
            fprintf(stderr, "mock_relay: warning: failed to load seed file %s\n",
                    server->seed_file);
        }
    }

    return server;
}

int nostr_mock_server_start(NostrMockRelayServer *server) {
    if (!server) return -1;
    if (server->running) return 0;  /* already running */

    /* Create libwebsockets context */
    struct lws_context_creation_info info = {0};
    info.port = server->config.port;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.user = server;
    info.options = LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;

    if (server->config.use_tls && server->cert_path && server->key_path) {
        info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        info.ssl_cert_filepath = server->cert_path;
        info.ssl_private_key_filepath = server->key_path;
    }

    server->lws_ctx = lws_create_context(&info);
    if (!server->lws_ctx) {
        fprintf(stderr, "mock_relay: failed to create lws context\n");
        return -1;
    }

    /* Get the actual port (especially important when port=0)
     * For libwebsockets 4.x+, we need to use a different approach to get the port.
     * lws_get_vhost_port returns the listening port of the vhost.
     */
    struct lws_vhost *vhost = lws_get_vhost_by_name(server->lws_ctx, "default");
    if (vhost) {
        server->actual_port = (uint16_t)lws_get_vhost_port(vhost);
    } else {
        /* Fallback: use configured port or probe the context */
        if (server->config.port != 0) {
            server->actual_port = server->config.port;
        } else {
            /* For port 0, libwebsockets will have assigned a port but we can't
             * easily retrieve it without a vhost reference. Use a workaround. */
            server->actual_port = server->config.port ? server->config.port : 9999;
        }
    }

    /* Build URL string */
    const char *scheme = server->config.use_tls ? "wss" : "ws";
    snprintf(server->url, sizeof(server->url), "%s://%s:%u",
             scheme, server->bind_addr, server->actual_port);

    /* Start service thread */
    server->should_stop = 0;
    server->running = 1;
    if (pthread_create(&server->service_thread, NULL, service_thread_func, server) != 0) {
        lws_context_destroy(server->lws_ctx);
        server->lws_ctx = NULL;
        server->running = 0;
        return -1;
    }

    /* Brief delay to ensure server is ready */
    usleep(50000);  /* 50ms */

    return 0;
}

void nostr_mock_server_stop(NostrMockRelayServer *server) {
    if (!server || !server->running) return;

    server->should_stop = 1;

    if (server->lws_ctx) {
        lws_cancel_service(server->lws_ctx);
    }

    /* Wait for service thread to exit */
    pthread_join(server->service_thread, NULL);

    /* Destroy context */
    if (server->lws_ctx) {
        lws_context_destroy(server->lws_ctx);
        server->lws_ctx = NULL;
    }

    server->running = 0;
}

void nostr_mock_server_free(NostrMockRelayServer *server) {
    if (!server) return;

    /* Stop if running */
    nostr_mock_server_stop(server);

    /* Free connections */
    nsync_mu_lock(&server->mutex);
    MockConnection *conn = server->connections;
    while (conn) {
        MockConnection *next = conn->next;
        for (int i = 0; i < RING_DEPTH; i++) {
            free(conn->pending[i]);
        }
        for (int i = 0; i < conn->sub_count; i++) {
            free(conn->subscriptions[i]);
        }
        free(conn);
        conn = next;
    }
    server->connections = NULL;

    /* Free seeded events */
    for (size_t i = 0; i < server->seeded_count; i++) {
        free(server->seeded_events[i]);
    }

    /* Free published events */
    for (size_t i = 0; i < server->published_count; i++) {
        free(server->published_events[i]);
    }
    nsync_mu_unlock(&server->mutex);

    /* Free strings */
    free(server->bind_addr);
    free(server->cert_path);
    free(server->key_path);
    free(server->seed_file);
    free(server->relay_name);
    free(server->relay_desc);
    free(server->nip11_json);

    free(server);
}

const char *nostr_mock_server_get_url(NostrMockRelayServer *server) {
    if (!server) return NULL;
    return server->url;
}

uint16_t nostr_mock_server_get_port(NostrMockRelayServer *server) {
    if (!server) return 0;
    return server->actual_port;
}

size_t nostr_mock_server_get_connection_count(NostrMockRelayServer *server) {
    if (!server) return 0;
    nsync_mu_lock(&server->mutex);
    size_t count = server->conn_count;
    nsync_mu_unlock(&server->mutex);
    return count;
}

int nostr_mock_server_seed_event(NostrMockRelayServer *server, const char *event_json) {
    if (!server || !event_json) return -1;

    /* Basic JSON validation */
    json_error_t error;
    json_t *root = json_loads(event_json, 0, &error);
    if (!root) {
        fprintf(stderr, "mock_relay: invalid event JSON: %s\n", error.text);
        return -1;
    }
    json_decref(root);

    nsync_mu_lock(&server->mutex);
    if (server->seeded_count >= MAX_EVENTS) {
        nsync_mu_unlock(&server->mutex);
        return -1;
    }
    server->seeded_events[server->seeded_count++] = strdup(event_json);
    nsync_mu_unlock(&server->mutex);

    return 0;
}

int nostr_mock_server_seed_from_jsonl(NostrMockRelayServer *server, const char *jsonl_path) {
    if (!server || !jsonl_path) return -1;

    FILE *f = fopen(jsonl_path, "r");
    if (!f) {
        fprintf(stderr, "mock_relay: cannot open seed file: %s\n", jsonl_path);
        return -1;
    }

    int loaded = 0;
    char line[MAX_MSG_SIZE];

    while (fgets(line, sizeof(line), f)) {
        /* Skip empty lines and comments */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '#') continue;

        /* Remove trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
        if (len > 1 && line[len-2] == '\r') line[len-2] = '\0';

        if (nostr_mock_server_seed_event(server, line) == 0) {
            loaded++;
        }
    }

    fclose(f);
    return loaded;
}

void nostr_mock_server_clear_events(NostrMockRelayServer *server) {
    if (!server) return;

    nsync_mu_lock(&server->mutex);
    for (size_t i = 0; i < server->seeded_count; i++) {
        free(server->seeded_events[i]);
        server->seeded_events[i] = NULL;
    }
    server->seeded_count = 0;
    nsync_mu_unlock(&server->mutex);
}

size_t nostr_mock_server_get_seeded_count(NostrMockRelayServer *server) {
    if (!server) return 0;
    nsync_mu_lock(&server->mutex);
    size_t count = server->seeded_count;
    nsync_mu_unlock(&server->mutex);
    return count;
}

char *nostr_mock_server_get_published_json(NostrMockRelayServer *server) {
    if (!server) return NULL;

    nsync_mu_lock(&server->mutex);
    if (server->published_count == 0) {
        nsync_mu_unlock(&server->mutex);
        return NULL;
    }

    json_t *arr = json_array();
    for (size_t i = 0; i < server->published_count; i++) {
        json_error_t err;
        json_t *ev = json_loads(server->published_events[i], 0, &err);
        if (ev) {
            json_array_append_new(arr, ev);
        }
    }
    nsync_mu_unlock(&server->mutex);

    char *result = json_dumps(arr, JSON_COMPACT);
    json_decref(arr);
    return result;
}

size_t nostr_mock_server_get_published_count(NostrMockRelayServer *server) {
    if (!server) return 0;
    nsync_mu_lock(&server->mutex);
    size_t count = server->published_count;
    nsync_mu_unlock(&server->mutex);
    return count;
}

void nostr_mock_server_clear_published(NostrMockRelayServer *server) {
    if (!server) return;

    nsync_mu_lock(&server->mutex);
    for (size_t i = 0; i < server->published_count; i++) {
        free(server->published_events[i]);
        server->published_events[i] = NULL;
    }
    server->published_count = 0;
    nsync_mu_unlock(&server->mutex);
}

char *nostr_mock_server_await_publish(NostrMockRelayServer *server, int timeout_ms) {
    if (!server) return NULL;

    nsync_mu_lock(&server->mutex);

    /* Check if we already have a published event */
    if (server->published_count > 0) {
        char *result = strdup(server->published_events[server->published_count - 1]);
        nsync_mu_unlock(&server->mutex);
        return result;
    }

    if (timeout_ms == 0) {
        nsync_mu_unlock(&server->mutex);
        return NULL;
    }

    /* Wait for a publish event */
    struct timespec deadline;
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (timeout_ms % 1000) * 1000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
    }

    size_t initial_count = server->published_count;
    while (server->published_count == initial_count) {
        if (timeout_ms < 0) {
            nsync_cv_wait(&server->cond_publish, &server->mutex);
        } else {
            int r = nsync_cv_wait_with_deadline(&server->cond_publish, &server->mutex,
                                                 deadline, NULL);
            if (r != 0) {
                /* Timeout */
                nsync_mu_unlock(&server->mutex);
                return NULL;
            }
        }
    }

    char *result = strdup(server->published_events[server->published_count - 1]);
    nsync_mu_unlock(&server->mutex);
    return result;
}

void nostr_mock_server_get_stats(NostrMockRelayServer *server, NostrMockRelayStats *stats) {
    if (!server || !stats) return;

    memset(stats, 0, sizeof(*stats));

    nsync_mu_lock(&server->mutex);
    stats->events_seeded = server->seeded_count;
    stats->events_matched = server->events_matched;
    stats->events_published = server->published_count;
    stats->subscriptions_received = server->subs_received;
    stats->close_received = server->close_received;
    stats->connections_total = server->conn_total;
    stats->connections_current = server->conn_count;
    nsync_mu_unlock(&server->mutex);
}

void nostr_mock_server_set_nip11_json(NostrMockRelayServer *server, const char *nip11_json) {
    if (!server) return;

    nsync_mu_lock(&server->mutex);
    free(server->nip11_json);
    server->nip11_json = nip11_json ? strdup(nip11_json) : NULL;
    nsync_mu_unlock(&server->mutex);
}

/* === Internal Implementation === */

static void *service_thread_func(void *arg) {
    NostrMockRelayServer *server = (NostrMockRelayServer *)arg;

    while (!server->should_stop) {
        lws_service(server->lws_ctx, 50);
    }

    return NULL;
}

static char *build_default_nip11(NostrMockRelayServer *server) {
    json_t *doc = json_object();
    json_object_set_new(doc, "name", json_string(server->relay_name));
    json_object_set_new(doc, "description", json_string(server->relay_desc));
    json_object_set_new(doc, "pubkey", json_string("0000000000000000000000000000000000000000000000000000000000000000"));
    json_object_set_new(doc, "contact", json_string("mock@test.local"));

    json_t *nips = json_array();
    json_array_append_new(nips, json_integer(1));
    json_array_append_new(nips, json_integer(11));
    json_object_set_new(doc, "supported_nips", nips);

    json_object_set_new(doc, "software", json_string("nostrc-mock-relay"));
    json_object_set_new(doc, "version", json_string("0.1.0"));

    char *result = json_dumps(doc, JSON_COMPACT);
    json_decref(doc);
    return result;
}

static int mock_http_callback(struct lws *wsi, enum lws_callback_reasons reason,
                              void *user, void *in, size_t len) {
    (void)user;

    switch (reason) {
        case LWS_CALLBACK_HTTP: {
            NostrMockRelayServer *server = (NostrMockRelayServer *)
                lws_context_user(lws_get_context(wsi));
            if (!server) break;

            /* Check for Accept: application/nostr+json header (NIP-11) */
            char accept[128] = {0};
            if (lws_hdr_total_length(wsi, WSI_TOKEN_HTTP_ACCEPT) > 0) {
                lws_hdr_copy(wsi, accept, sizeof(accept), WSI_TOKEN_HTTP_ACCEPT);
            }

            if (strstr(accept, "application/nostr+json")) {
                /* Serve NIP-11 document */
                nsync_mu_lock(&server->mutex);
                char *nip11 = server->nip11_json ? strdup(server->nip11_json)
                                                  : build_default_nip11(server);
                nsync_mu_unlock(&server->mutex);

                size_t nip11_len = strlen(nip11);

                unsigned char *buf = malloc(LWS_PRE + 512 + nip11_len);
                if (!buf) {
                    free(nip11);
                    return -1;
                }

                unsigned char *start = buf + LWS_PRE;
                unsigned char *p = start;
                unsigned char *end = buf + LWS_PRE + 512 + nip11_len;

                if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end)) {
                    free(buf);
                    free(nip11);
                    return -1;
                }
                if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                        (unsigned char *)"application/nostr+json", 22, &p, end)) {
                    free(buf);
                    free(nip11);
                    return -1;
                }
                if (lws_add_http_header_content_length(wsi, nip11_len, &p, end)) {
                    free(buf);
                    free(nip11);
                    return -1;
                }
                if (lws_finalize_write_http_header(wsi, start, &p, end)) {
                    free(buf);
                    free(nip11);
                    return -1;
                }

                /* Write body */
                memcpy(p, nip11, nip11_len);
                lws_write(wsi, p, nip11_len, LWS_WRITE_HTTP_FINAL);

                free(buf);
                free(nip11);
                return -1;  /* close connection */
            }
            break;
        }
        default:
            break;
    }

    return lws_callback_http_dummy(wsi, reason, user, in, len);
}

static int mock_ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
                            void *user, void *in, size_t len) {
    NostrMockRelayServer *server = (NostrMockRelayServer *)
        lws_context_user(lws_get_context(wsi));
    MockConnection **conn_ptr = (MockConnection **)user;

    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED: {
            if (!server) break;

            /* Allocate connection state */
            MockConnection *conn = calloc(1, sizeof(MockConnection));
            if (!conn) return -1;

            conn->wsi = wsi;
            *conn_ptr = conn;

            /* Add to connections list */
            nsync_mu_lock(&server->mutex);
            conn->next = server->connections;
            server->connections = conn;
            server->conn_count++;
            server->conn_total++;
            nsync_mu_unlock(&server->mutex);

            break;
        }

        case LWS_CALLBACK_CLOSED: {
            if (!server || !conn_ptr || !*conn_ptr) break;

            MockConnection *conn = *conn_ptr;

            /* Remove from connections list */
            nsync_mu_lock(&server->mutex);
            MockConnection **pp = &server->connections;
            while (*pp) {
                if (*pp == conn) {
                    *pp = conn->next;
                    server->conn_count--;
                    break;
                }
                pp = &(*pp)->next;
            }
            nsync_mu_unlock(&server->mutex);

            /* Free connection resources */
            for (int i = 0; i < RING_DEPTH; i++) {
                free(conn->pending[i]);
            }
            for (int i = 0; i < conn->sub_count; i++) {
                free(conn->subscriptions[i]);
            }
            free(conn);
            *conn_ptr = NULL;
            break;
        }

        case LWS_CALLBACK_RECEIVE: {
            if (!server || !conn_ptr || !*conn_ptr || !in || len == 0) break;

            MockConnection *conn = *conn_ptr;
            const char *msg = (const char *)in;

            /* Parse envelope type from JSON array */
            if (len < 8 || msg[0] != '[') break;

            /* Add response delay if configured */
            if (server->config.response_delay_ms > 0) {
                usleep((useconds_t)server->config.response_delay_ms * 1000);
            }

            if (strncmp(msg + 1, "\"REQ\"", 5) == 0) {
                handle_req_envelope(server, conn, msg, len);
            } else if (strncmp(msg + 1, "\"EVENT\"", 7) == 0) {
                handle_event_envelope(server, conn, msg, len);
            } else if (strncmp(msg + 1, "\"CLOSE\"", 7) == 0) {
                handle_close_envelope(server, conn, msg, len);
            }
            /* Ignore unknown message types */
            break;
        }

        case LWS_CALLBACK_SERVER_WRITEABLE: {
            if (!conn_ptr || !*conn_ptr) break;

            MockConnection *conn = *conn_ptr;

            /* Send next pending message from ring buffer */
            if (conn->ring_count > 0) {
                int idx = conn->ring_tail;
                char *msg = conn->pending[idx];
                size_t msg_len = conn->pending_len[idx];

                if (msg && msg_len > 0) {
                    unsigned char *buf = malloc(LWS_PRE + msg_len);
                    if (buf) {
                        memcpy(buf + LWS_PRE, msg, msg_len);
                        lws_write(wsi, buf + LWS_PRE, msg_len, LWS_WRITE_TEXT);
                        free(buf);
                    }
                    free(msg);
                    conn->pending[idx] = NULL;
                }

                conn->ring_tail = (conn->ring_tail + 1) % RING_DEPTH;
                conn->ring_count--;

                /* More messages pending? Request another writable callback */
                if (conn->ring_count > 0) {
                    lws_callback_on_writable(wsi);
                }
            }
            break;
        }

        default:
            break;
    }

    return 0;
}

static void send_to_connection(MockConnection *conn, const char *msg) {
    if (!conn || !msg || conn->ring_count >= RING_DEPTH) return;

    int idx = conn->ring_head;
    conn->pending[idx] = strdup(msg);
    conn->pending_len[idx] = strlen(msg);
    conn->ring_head = (conn->ring_head + 1) % RING_DEPTH;
    conn->ring_count++;

    lws_callback_on_writable(conn->wsi);
}

static void handle_req_envelope(NostrMockRelayServer *server, MockConnection *conn,
                                 const char *msg, size_t len) {
    /* Parse: ["REQ", "<sub_id>", <filter>, ...] */
    json_error_t err;
    json_t *root = json_loadb(msg, len, 0, &err);
    if (!root || !json_is_array(root) || json_array_size(root) < 3) {
        json_decref(root);
        return;
    }

    const char *sub_id = json_string_value(json_array_get(root, 1));
    if (!sub_id) {
        json_decref(root);
        return;
    }

    nsync_mu_lock(&server->mutex);
    server->subs_received++;

    /* Store subscription ID */
    if (conn->sub_count < 16) {
        conn->subscriptions[conn->sub_count++] = strdup(sub_id);
    }

    /* Collect filters */
    size_t filter_count = json_array_size(root) - 2;
    json_t **filters = malloc(filter_count * sizeof(json_t *));
    for (size_t i = 0; i < filter_count; i++) {
        filters[i] = json_array_get(root, i + 2);
    }

    /* Match against seeded events */
    int matched = 0;
    int max_events = server->config.max_events_per_req;

    for (size_t i = 0; i < server->seeded_count; i++) {
        if (max_events >= 0 && matched >= max_events) break;

        const char *event_json = server->seeded_events[i];

        /* Check each filter */
        bool matches = false;
        for (size_t f = 0; f < filter_count && !matches; f++) {
            char *filter_str = json_dumps(filters[f], JSON_COMPACT);
            if (filter_str) {
                matches = filter_matches_event_json(filter_str, event_json);
                free(filter_str);
            }
        }

        if (matches) {
            /* Send EVENT envelope: ["EVENT", "<sub_id>", <event>] */
            size_t envelope_size = strlen(event_json) + strlen(sub_id) + 32;
            char *envelope = malloc(envelope_size);
            if (envelope) {
                snprintf(envelope, envelope_size, "[\"EVENT\",\"%s\",%s]", sub_id, event_json);
                send_to_connection(conn, envelope);
                free(envelope);
                matched++;
                server->events_matched++;
            }
        }
    }

    free(filters);
    nsync_mu_unlock(&server->mutex);

    /* Send EOSE if configured */
    if (server->config.auto_eose) {
        char eose[256];
        snprintf(eose, sizeof(eose), "[\"EOSE\",\"%s\"]", sub_id);
        send_to_connection(conn, eose);
    }

    json_decref(root);
}

static void handle_event_envelope(NostrMockRelayServer *server, MockConnection *conn,
                                   const char *msg, size_t len) {
    /* Parse: ["EVENT", <event>] */
    json_error_t err;
    json_t *root = json_loadb(msg, len, 0, &err);
    if (!root || !json_is_array(root) || json_array_size(root) < 2) {
        json_decref(root);
        return;
    }

    json_t *event = json_array_get(root, 1);
    if (!json_is_object(event)) {
        json_decref(root);
        return;
    }

    /* Extract event ID */
    const char *event_id = json_string_value(json_object_get(event, "id"));
    if (!event_id) event_id = "unknown";

    /* Serialize event to JSON string */
    char *event_str = json_dumps(event, JSON_COMPACT);

    /* Validate signature if configured */
    bool valid = true;
    if (server->config.validate_signatures && event_str) {
        NostrEvent *nostr_event = nostr_event_new();
        if (nostr_event) {
            if (nostr_event_deserialize_compact(nostr_event, event_str, NULL) == 1) {
                valid = nostr_event_check_signature(nostr_event);
            } else {
                valid = false;  /* Failed to parse event */
            }
            nostr_event_free(nostr_event);
        } else {
            valid = false;  /* Failed to allocate event */
        }
    }

    /* Only store published event if valid (or validation disabled) */
    if (!valid) {
        free(event_str);
        event_str = NULL;
    }

    nsync_mu_lock(&server->mutex);
    if (server->published_count < MAX_EVENTS && event_str) {
        server->published_events[server->published_count++] = event_str;
        nsync_cv_broadcast(&server->cond_publish);
    } else {
        free(event_str);
    }
    nsync_mu_unlock(&server->mutex);

    /* Send OK response: ["OK", "<event_id>", true/false, "<message>"] */
    char ok_msg[512];
    if (valid) {
        snprintf(ok_msg, sizeof(ok_msg), "[\"OK\",\"%s\",true,\"\"]", event_id);
    } else {
        snprintf(ok_msg, sizeof(ok_msg), "[\"OK\",\"%s\",false,\"invalid: signature verification failed\"]", event_id);
    }
    send_to_connection(conn, ok_msg);

    json_decref(root);
}

static void handle_close_envelope(NostrMockRelayServer *server, MockConnection *conn,
                                   const char *msg, size_t len) {
    /* Parse: ["CLOSE", "<sub_id>"] */
    json_error_t err;
    json_t *root = json_loadb(msg, len, 0, &err);
    if (!root || !json_is_array(root) || json_array_size(root) < 2) {
        json_decref(root);
        return;
    }

    const char *sub_id = json_string_value(json_array_get(root, 1));
    if (!sub_id) {
        json_decref(root);
        return;
    }

    nsync_mu_lock(&server->mutex);
    server->close_received++;

    /* Remove subscription from connection */
    for (int i = 0; i < conn->sub_count; i++) {
        if (conn->subscriptions[i] && strcmp(conn->subscriptions[i], sub_id) == 0) {
            free(conn->subscriptions[i]);
            /* Shift remaining subscriptions */
            for (int j = i; j < conn->sub_count - 1; j++) {
                conn->subscriptions[j] = conn->subscriptions[j + 1];
            }
            conn->sub_count--;
            break;
        }
    }
    nsync_mu_unlock(&server->mutex);

    json_decref(root);
}

/**
 * Simplified filter matching for JSON strings.
 * In a real implementation, this would use nostr_filter_matches().
 */
static bool filter_matches_event_json(const char *filter_json, const char *event_json) {
    json_error_t err;
    json_t *filter = json_loads(filter_json, 0, &err);
    json_t *event = json_loads(event_json, 0, &err);

    if (!filter || !event) {
        json_decref(filter);
        json_decref(event);
        return false;
    }

    bool matches = true;

    /* Check IDs */
    json_t *ids = json_object_get(filter, "ids");
    if (json_is_array(ids) && json_array_size(ids) > 0) {
        const char *event_id = json_string_value(json_object_get(event, "id"));
        if (!event_id) { matches = false; goto done; }

        bool found = false;
        for (size_t i = 0; i < json_array_size(ids); i++) {
            const char *fid = json_string_value(json_array_get(ids, i));
            if (fid && strncmp(event_id, fid, strlen(fid)) == 0) {
                found = true;
                break;
            }
        }
        if (!found) { matches = false; goto done; }
    }

    /* Check kinds */
    json_t *kinds = json_object_get(filter, "kinds");
    if (json_is_array(kinds) && json_array_size(kinds) > 0) {
        json_int_t event_kind = json_integer_value(json_object_get(event, "kind"));

        bool found = false;
        for (size_t i = 0; i < json_array_size(kinds); i++) {
            if (json_integer_value(json_array_get(kinds, i)) == event_kind) {
                found = true;
                break;
            }
        }
        if (!found) { matches = false; goto done; }
    }

    /* Check authors */
    json_t *authors = json_object_get(filter, "authors");
    if (json_is_array(authors) && json_array_size(authors) > 0) {
        const char *pubkey = json_string_value(json_object_get(event, "pubkey"));
        if (!pubkey) { matches = false; goto done; }

        bool found = false;
        for (size_t i = 0; i < json_array_size(authors); i++) {
            const char *author = json_string_value(json_array_get(authors, i));
            if (author && strncmp(pubkey, author, strlen(author)) == 0) {
                found = true;
                break;
            }
        }
        if (!found) { matches = false; goto done; }
    }

    /* Check since/until timestamps */
    json_t *since = json_object_get(filter, "since");
    json_t *until = json_object_get(filter, "until");
    json_int_t created_at = json_integer_value(json_object_get(event, "created_at"));

    if (json_is_integer(since) && created_at < json_integer_value(since)) {
        matches = false;
        goto done;
    }
    if (json_is_integer(until) && created_at > json_integer_value(until)) {
        matches = false;
        goto done;
    }

    /* Check tag filters (#e, #p, etc.) */
    json_t *event_tags = json_object_get(event, "tags");
    const char *key;
    json_t *value;
    json_object_foreach(filter, key, value) {
        if (key[0] == '#' && strlen(key) == 2 && json_is_array(value)) {
            char tag_key[2] = { key[1], '\0' };

            /* Check if event has any matching tag */
            bool found = false;
            if (json_is_array(event_tags)) {
                for (size_t i = 0; i < json_array_size(event_tags) && !found; i++) {
                    json_t *tag = json_array_get(event_tags, i);
                    if (json_is_array(tag) && json_array_size(tag) >= 2) {
                        const char *tk = json_string_value(json_array_get(tag, 0));
                        const char *tv = json_string_value(json_array_get(tag, 1));
                        if (tk && tv && strcmp(tk, tag_key) == 0) {
                            /* Check if tag value is in filter array */
                            for (size_t j = 0; j < json_array_size(value); j++) {
                                const char *fv = json_string_value(json_array_get(value, j));
                                if (fv && strcmp(tv, fv) == 0) {
                                    found = true;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            if (!found && json_array_size(value) > 0) {
                matches = false;
                goto done;
            }
        }
    }

done:
    json_decref(filter);
    json_decref(event);
    return matches;
}
