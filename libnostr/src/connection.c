#include <libwebsockets.h>
#include <zlib.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "connection-private.h"

static const struct lws_protocols protocols[] = {
    {
        .name = "nostr",
        .callback = NULL, // Provide the appropriate callback here
        .per_session_data_size = 0,
        .rx_buffer_size = 0,
    },
    { NULL, NULL, 0, 0 }
};

Connection* new_connection(const char *url, int port) {
    struct lws_context_creation_info ctx_info;
    struct lws_client_connect_info cc_info;
    struct lws_context *context;
    
    Connection *conn = malloc(sizeof(Connection));
    ConnectionPrivate *priv = malloc(sizeof(ConnectionPrivate));
    if (!conn || !priv) return NULL;
    conn->priv = priv;

    // Initialize context creation info
    memset(&ctx_info, 0, sizeof(ctx_info));
    ctx_info.port = CONTEXT_PORT_NO_LISTEN;
    ctx_info.protocols = protocols;
    ctx_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    context = lws_create_context(&ctx_info);
    if (!context) {
        free(conn);
        return NULL;
    }

    // Initialize client connect info
    memset(&cc_info, 0, sizeof(cc_info));
    cc_info.context = context;
    cc_info.address = url;
    cc_info.port = port;
    cc_info.path = "/";
    cc_info.host = lws_canonical_hostname(context);
    cc_info.origin = "origin";
    cc_info.ssl_connection = LCCSCF_USE_SSL;
    cc_info.protocol = protocols[0].name;

    conn->priv->wsi = lws_client_connect_via_info(&cc_info);
    if (!conn->priv->wsi) {
        lws_context_destroy(context);
        free(conn);
        return NULL;
    }

    conn->priv->enable_compression = 1; // Assuming compression is enabled
    inflateInit(&conn->priv->zstream);
    return conn;
}


void init_compression(Connection *conn) {
    memset(&conn->priv->zstream, 0, sizeof(conn->priv->zstream));
    inflateInit(&conn->priv->zstream);
}

void decompress_message(Connection *conn, const char *input, char *output, size_t input_len, size_t *output_len) {
    conn->priv->zstream.avail_in = input_len;
    conn->priv->zstream.next_in = (unsigned char *)input;
    conn->priv->zstream.avail_out = *output_len;
    conn->priv->zstream.next_out = (unsigned char *)output;

    inflate(&conn->priv->zstream, Z_FINISH);
    *output_len = conn->priv->zstream.total_out;
    inflateReset(&conn->priv->zstream);
}

int write_message(Connection *conn, const char *message, size_t len) {
    unsigned char buf[LWS_PRE + len];
    memcpy(&buf[LWS_PRE], message, len);

    int n = lws_write(conn->priv->wsi, &buf[LWS_PRE], len, LWS_WRITE_TEXT);
    return n;
}

int read_message(Connection *conn, char *buffer, size_t buffer_len) {
    struct lws_pollfd fds;
    int n;

    fds.fd = lws_get_socket_fd(conn->wsi);
    fds.events = POLLIN;
    fds.revents = 0;

    n = poll(&fds, 1, 1000);
    if (n > 0) {
        lws_service_fd(conn->priv->wsi->context, &fds);
        memcpy(buffer, conn->priv->zstream.next_out, conn->priv->zstream.total_out);
        return conn->priv->zstream.total_out;
    }

    return 0;
}
