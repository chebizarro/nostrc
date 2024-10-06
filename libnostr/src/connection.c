#include "nostr.h"
#include "connection-private.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_PAYLOAD_SIZE 1024

static int websocket_callback(struct lws *wsi, enum lws_callback_reasons reason,
                              void *user, void *in, size_t len) {
	Connection *conn = (Connection*)user;

    switch (reason) {
        case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
            // Add custom headers (e.g., User-Agent) before the WebSocket handshake
            unsigned char **p = (unsigned char **)in;
            unsigned char *end = (*p) + len;

            const char *user_agent = "User-Agent: nostrc/1.0\r\n";
            if (lws_add_http_header_by_name(wsi, (unsigned char *)"User-Agent:",
                                            (unsigned char *)user_agent,
                                            strlen(user_agent), p, end)) {
                return 1;
            }
            break;
        }
        case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP:
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            printf("WebSocket connection established\n");
            break;
        case LWS_CALLBACK_CLIENT_RECEIVE:
            conn->received_message = malloc(len + 1);  // Allocate memory for the message
            if (conn->received_message) {
                memcpy(conn->received_message, in, len);  // Copy the received data
                conn->received_message[len] = '\0';  // Null-terminate the string
            }
            break;
        case LWS_CALLBACK_CLIENT_WRITEABLE:
            // Handle message writing (equivalent to WriteMessage)
            if (conn->message_to_write && conn->message_length > 0) {
                unsigned char buf[LWS_PRE + MAX_PAYLOAD_SIZE];
                unsigned char *p = &buf[LWS_PRE];
                size_t n = conn->message_length > MAX_PAYLOAD_SIZE ? MAX_PAYLOAD_SIZE : conn->message_length;

                // Copy the message to the buffer
                memcpy(p, conn->message_to_write, n);

                // Write the message to the WebSocket
                int m = lws_write(wsi, p, n, LWS_WRITE_TEXT);
                if (m < 0) {
                    fprintf(stderr, "Error writing to WebSocket\n");
                    return -1;
                }

                // Update the message length and remaining message to write
                conn->message_length -= n;
                conn->message_to_write += n;

                // If there's more to write, mark the connection as writeable again
                if (conn->message_length > 0) {
                    lws_callback_on_writable(wsi);
                }
            }
            break;
        case LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH: {
            // Check if the server supports the "permessage-deflate" extension
            const unsigned char *exts = (const unsigned char *)lws_hdr_simple_ptr(wsi, WSI_TOKEN_EXTENSIONS);
            if (exts && strstr((const char *)exts, "permessage-deflate")) {
                printf("Server supports permessage-deflate\n");
                conn->priv->enable_compression = 1;
            } else {
                printf("Server does not support permessage-deflate\n");
                conn->priv->enable_compression = 0;
            }
            break;
        }
        case LWS_CALLBACK_CLIENT_CLOSED:
		case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
			connection_close(conn);
            break;
        default:
            break;
    }
    return 0;
}

static const struct lws_protocols protocols[] = {
    { "wss", websocket_callback, 0, 128 },
    LWS_PROTOCOL_LIST_TERM
};

static const struct lws_extension extensions[] = {
    { "permessage-deflate", lws_extension_callback_pm_deflate, "permessage-deflate; client_no_context_takeover; client_max_window_bits" },
	{ NULL, NULL, NULL }
};

Connection* new_connection(const char *url, int port) {
	struct lws_context_creation_info context_info;
    struct lws_client_connect_info connect_info;
    struct lws_context *context;
    
    Connection *conn = malloc(sizeof(Connection));
    ConnectionPrivate *priv = malloc(sizeof(ConnectionPrivate));
    if (!conn || !priv) return NULL;
    conn->priv = priv;
	conn->priv->enable_compression = 0; 

    // Initialize context creation info
    memset(&connect_info, 0, sizeof(connect_info));
	context_info.port = CONTEXT_PORT_NO_LISTEN;
	context_info.protocols = protocols;
	context_info.gid = -1;
	context_info.uid = -1;

    context = lws_create_context(&context_info);
    if (!context) {
        free(conn);
        return NULL;
    }

    // Initialize client connect info
    memset(&connect_info, 0, sizeof(connect_info));
    connect_info.context = context;
    connect_info.address = url;
    connect_info.port = port;
    connect_info.path = "/";
    connect_info.host = lws_canonical_hostname(context);
    connect_info.origin = connect_info.host;
    connect_info.ssl_connection = LCCSCF_USE_SSL;
    connect_info.protocol = "wss";
    connect_info.pwsi = &conn->priv->wsi;
	connect_info.userdata = conn;

    if (conn->priv->enable_compression) {
        connect_info.extensions = extensions;
    }

    conn->priv->wsi = lws_client_connect_via_info(&connect_info);
    if (!conn->priv->wsi) {
        lws_context_destroy(context);
        free(conn);
        return NULL;
    }

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

int write_message(Connection *conn, const char *message) {
    unsigned char buf[LWS_SEND_BUFFER_PRE_PADDING + 512 + LWS_SEND_BUFFER_POST_PADDING];
    unsigned char *p = &buf[LWS_SEND_BUFFER_PRE_PADDING];
    size_t n = strlen(message);

    memcpy(p, message, n);
    lws_write(conn->priv->wsi, p, n, LWS_WRITE_TEXT);

    return 0;
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

void connection_close(Connection *conn) {
    lws_context_destroy(conn->priv->context);
    free(conn);
}
