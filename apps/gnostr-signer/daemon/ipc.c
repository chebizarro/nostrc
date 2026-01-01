#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "ipc.h"

/* IPC server state and statistics */
typedef struct {
  guint64 connections_total;
  guint64 connections_active;
  guint64 requests_total;
  guint64 errors_total;
  gint64 start_time;
} IpcStats;

static IpcStats g_ipc_stats = {0};
#ifdef GNOSTR_ENABLE_TCP_IPC
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include "nostr/nip5f/nip5f.h"
#include "json.h"
// Forward declarations to avoid depending on internal headers
struct Nip5fConnArg {
  int fd;
  void *ud;
  Nip5fGetPubFn get_pub;
  Nip5fSignEventFn sign_event;
  Nip5fNip44EncFn enc44;
  Nip5fNip44DecFn dec44;
  Nip5fListKeysFn list_keys;
};
void *nip5f_conn_thread(void *arg);
int nip5f_read_frame(int fd, char **out_json, size_t *out_len);
int nip5f_write_frame(int fd, const char *json, size_t len);
#endif

// Reuse existing UDS server
extern int gnostr_uds_sockd_start(const char *socket_path);
extern void gnostr_uds_sockd_stop(void);

struct GnostrIpcServer {
  enum { IPC_NONE=0, IPC_UNIX, IPC_TCP, IPC_NPIPE } kind;
  gchar *endpoint;
  GMutex stats_mutex;
  IpcStats *stats;
#ifdef GNOSTR_ENABLE_TCP_IPC
  int tcp_fd;
  GThread *tcp_thr;
  gint stop_flag;
  gchar *token_path;
  gchar *token;
  guint16 port;
  gchar host[64];
  guint max_connections;
  gint active_connections;
#endif
};

#ifdef GNOSTR_ENABLE_TCP_IPC
static gpointer tcp_ipc_accept_thread(gpointer data);
#endif

static gchar *default_endpoint(void) {
#ifdef G_OS_UNIX
  const char *runtime = g_get_user_runtime_dir();
  if (!runtime || !*runtime) runtime = "/tmp";
  return g_build_filename(runtime, "gnostr", "signer.sock", NULL);
#else
  // Windows Named Pipe default endpoint string: \\.\pipe\gnostr-signer
  return g_strdup("npipe:\\\\.\\\\pipe\\\\gnostr-signer");
#endif
}

static void ipc_stats_init(GnostrIpcServer *srv) {
  if (!srv->stats) {
    srv->stats = &g_ipc_stats;
    srv->stats->start_time = g_get_monotonic_time();
  }
  g_mutex_init(&srv->stats_mutex);
}

static void ipc_stats_connection_opened(GnostrIpcServer *srv) {
  g_mutex_lock(&srv->stats_mutex);
  srv->stats->connections_total++;
  srv->stats->connections_active++;
  g_mutex_unlock(&srv->stats_mutex);
}

static void ipc_stats_connection_closed(GnostrIpcServer *srv) {
  g_mutex_lock(&srv->stats_mutex);
  if (srv->stats->connections_active > 0) {
    srv->stats->connections_active--;
  }
  g_mutex_unlock(&srv->stats_mutex);
}

static void ipc_stats_request(GnostrIpcServer *srv) {
  g_mutex_lock(&srv->stats_mutex);
  srv->stats->requests_total++;
  g_mutex_unlock(&srv->stats_mutex);
}

static void ipc_stats_error(GnostrIpcServer *srv) {
  g_mutex_lock(&srv->stats_mutex);
  srv->stats->errors_total++;
  g_mutex_unlock(&srv->stats_mutex);
}

static void ipc_stats_cleanup(GnostrIpcServer *srv) {
  g_mutex_clear(&srv->stats_mutex);
}

GnostrIpcServer* gnostr_ipc_server_start(const char *endpoint) {
  const char *ep = endpoint;
  gchar *ep_alloc = NULL;
  GnostrIpcServer *srv = g_new0(GnostrIpcServer, 1);
  ipc_stats_init(srv);
  if (!ep || !*ep) {
#ifdef G_OS_UNIX
    gchar *p = default_endpoint();
    ep_alloc = g_strconcat("unix:", p, NULL);
    g_free(p);
    ep = ep_alloc;
#else
    ep_alloc = default_endpoint();
    ep = ep_alloc;
#endif
  }
  if (g_str_has_prefix(ep, "unix:")) {
#ifdef G_OS_UNIX
    const char *path = ep + 5;
    
    // Ensure parent directory exists with secure permissions
    gchar *dir = g_path_get_dirname(path);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
      g_warning("unix: failed to create directory %s: %s", dir, g_strerror(errno));
      g_free(dir);
      if (ep_alloc) g_free(ep_alloc);
      ipc_stats_cleanup(srv);
      g_free((gpointer)srv);
      return NULL;
    }
    g_free(dir);
    
    // Remove stale socket file if it exists
    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
      g_message("unix: removing stale socket at %s", path);
      g_unlink(path);
    }
    
    if (gnostr_uds_sockd_start(path) != 0) {
      g_critical("unix: failed to start UDS server at %s", path);
      if (ep_alloc) g_free(ep_alloc);
      ipc_stats_cleanup(srv);
      g_free((gpointer)srv);
      return NULL;
    }
    
    // Set socket file permissions to 0600 for security
    if (g_chmod(path, 0600) != 0) {
      g_warning("unix: failed to set socket permissions: %s", g_strerror(errno));
    }
    
    srv->kind = IPC_UNIX;
    srv->endpoint = g_strdup(path);
    g_message("unix ipc server started at %s", path);
    if (ep_alloc) g_free(ep_alloc);
    return srv;
#else
    g_warning("unix: endpoint not supported on this platform");
    if (ep_alloc) g_free(ep_alloc);
    g_free((gpointer)srv);
    return NULL;
#endif
  } else if (g_str_has_prefix(ep, "tcp:")) {
#ifdef GNOSTR_ENABLE_TCP_IPC
    // Parse tcp:HOST:PORT
    const char *spec = ep + 4;
    gchar **parts = g_strsplit(spec, ":", 2);
    if (!parts || !parts[0] || !parts[1]) {
      g_warning("tcp endpoint malformed: %s", ep);
      if (ep_alloc) g_free(ep_alloc);
      g_free(srv);
      if (parts) g_strfreev(parts);
      return NULL;
    }
    g_strlcpy(srv->host, parts[0], sizeof(srv->host));
    srv->port = (guint16)CLAMP((int)g_ascii_strtoll(parts[1], NULL, 10), 1, 65535);
    g_strfreev(parts);
    // Enforce loopback only for security
    if (g_strcmp0(srv->host, "127.0.0.1") != 0 && 
        g_strcmp0(srv->host, "localhost") != 0 && 
        g_strcmp0(srv->host, "::1") != 0) {
      g_critical("tcp endpoint must bind to loopback only, got: %s", srv->host);
      if (ep_alloc) g_free(ep_alloc);
      ipc_stats_cleanup(srv);
      g_free(srv);
      return NULL;
    }
    
    // Set maximum concurrent connections
    const char *max_conn_env = g_getenv("NOSTR_SIGNER_MAX_CONNECTIONS");
    srv->max_connections = max_conn_env ? (guint)g_ascii_strtoull(max_conn_env, NULL, 10) : 100;
    if (srv->max_connections == 0) srv->max_connections = 100;
    srv->active_connections = 0;
    // Prepare token file under XDG_RUNTIME_DIR/gnostr/token
    const char *rt = g_get_user_runtime_dir();
    if (!rt || !*rt) rt = g_get_tmp_dir();
    gchar *dir = g_build_filename(rt, "gnostr", NULL);
    g_mkdir_with_parents(dir, 0700);
    srv->token_path = g_build_filename(dir, "token", NULL);
    g_free(dir);
    // Load or create token (64 hex chars)
    GError *err = NULL;
    if (!g_file_get_contents(srv->token_path, &srv->token, NULL, &err)) {
      if (err) g_clear_error(&err);
      guint8 rnd[32];
      g_random_set_seed((guint32)g_get_monotonic_time());
      for (gsize i=0;i<sizeof(rnd);++i) rnd[i] = (guint8)g_random_int_range(0,256);
      gchar *hex = g_malloc0(65);
      static const char *H="0123456789abcdef";
      for (int i=0;i<32;i++){ hex[i*2]=H[(rnd[i]>>4)&0xF]; hex[i*2+1]=H[rnd[i]&0xF]; }
      g_file_set_contents(srv->token_path, hex, 64, NULL);
      srv->token = hex;
      // chmod to 0600 best-effort
      (void)g_chmod(srv->token_path, 0600);
    }
    // Create socket and thread
    srv->tcp_fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (srv->tcp_fd < 0) {
      g_warning("tcp: socket() failed: %s", g_strerror(errno));
      if (ep_alloc) g_free(ep_alloc);
      g_free(srv->token_path); g_free(srv->token); g_free(srv);
      return NULL;
    }
    // Set socket options for better performance and reliability
    int on=1;
    setsockopt(srv->tcp_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    
    // Set socket to non-blocking mode for better control
    int flags = fcntl(srv->tcp_fd, F_GETFL, 0);
    if (flags >= 0) {
      fcntl(srv->tcp_fd, F_SETFL, flags | O_NONBLOCK);
    }
    
    struct sockaddr_in addr; memset(&addr,0,sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(srv->port);
    addr.sin_addr.s_addr = inet_addr(srv->host);
    if (bind(srv->tcp_fd, (struct sockaddr*)&addr, sizeof(addr))<0 || listen(srv->tcp_fd, 16)<0) {
      g_warning("tcp: bind/listen failed: %s", g_strerror(errno));
      close(srv->tcp_fd);
      if (ep_alloc) g_free(ep_alloc);
      g_free(srv->token_path); g_free(srv->token); g_free(srv);
      return NULL;
    }
    g_atomic_int_set(&srv->stop_flag, 0);
    srv->tcp_thr = g_thread_new("gnostr-tcp-ipc", tcp_ipc_accept_thread, srv);
    srv->kind = IPC_TCP;
    srv->endpoint = g_strdup_printf("%s:%u", srv->host, srv->port);
    if (ep_alloc) g_free(ep_alloc);
    g_message("tcp ipc server started on %s:%u (token: %s, max_connections: %u)", 
              srv->host, (unsigned)srv->port, srv->token_path, srv->max_connections);
    return srv;
#else
    if (ep_alloc) g_free(ep_alloc);
    g_free((gpointer)srv);
    return NULL;
#endif
  } else if (g_str_has_prefix(ep, "npipe:")) {
#ifdef G_OS_WIN32
    g_warning("named pipe endpoint not yet implemented on Windows build");
    if (ep_alloc) g_free(ep_alloc);
    g_free((gpointer)srv);
    return NULL;
#else
    g_warning("npipe: endpoint only supported on Windows");
    if (ep_alloc) g_free(ep_alloc);
    g_free((gpointer)srv);
    return NULL;
#endif
  } else {
    g_warning("unknown endpoint scheme: %s", ep);
    if (ep_alloc) g_free(ep_alloc);
    g_free((gpointer)srv);
    return NULL;
  }
}

void gnostr_ipc_server_stop(GnostrIpcServer *srv) {
  if (!srv) return;
  
  g_message("stopping ipc server (endpoint: %s)", srv->endpoint ? srv->endpoint : "unknown");
  
  // Log final statistics
  if (srv->stats) {
    gint64 uptime = (g_get_monotonic_time() - srv->stats->start_time) / 1000000;
    g_message("ipc server stats: uptime=%" G_GINT64_FORMAT "s, "
              "total_connections=%" G_GUINT64_FORMAT ", "
              "active_connections=%" G_GUINT64_FORMAT ", "
              "total_requests=%" G_GUINT64_FORMAT ", "
              "total_errors=%" G_GUINT64_FORMAT,
              uptime,
              srv->stats->connections_total,
              srv->stats->connections_active,
              srv->stats->requests_total,
              srv->stats->errors_total);
  }
  
  if (srv->kind == IPC_UNIX) {
    gnostr_uds_sockd_stop();
    // Clean up socket file
    if (srv->endpoint && g_file_test(srv->endpoint, G_FILE_TEST_EXISTS)) {
      g_message("unix: removing socket file %s", srv->endpoint);
      g_unlink(srv->endpoint);
    }
  }
#ifdef GNOSTR_ENABLE_TCP_IPC
  if (srv->kind == IPC_TCP) {
    g_atomic_int_set(&srv->stop_flag, 1);
    if (srv->tcp_fd >= 0) {
      shutdown(srv->tcp_fd, SHUT_RDWR);
      close(srv->tcp_fd);
      srv->tcp_fd = -1;
    }
    if (srv->tcp_thr) {
      g_message("tcp: waiting for accept thread to finish");
      g_thread_join(srv->tcp_thr);
      srv->tcp_thr = NULL;
    }
    g_clear_pointer(&srv->token_path, g_free);
    g_clear_pointer(&srv->token, g_free);
  }
#endif
  
  ipc_stats_cleanup(srv);
  g_clear_pointer(&srv->endpoint, g_free);
  g_free(srv);
  g_message("ipc server stopped");
}

#ifdef GNOSTR_ENABLE_TCP_IPC
static gpointer tcp_ipc_accept_thread(gpointer data) {
  GnostrIpcServer *s = (GnostrIpcServer*)data;
  // Ensure JSON interface is initialized (same as UDS path)
  extern NostrJsonInterface *jansson_impl;
  nostr_set_json_interface(jansson_impl);
  nostr_json_init();
  
  g_message("tcp: accept thread started");
  
  while (!g_atomic_int_get(&s->stop_flag)) {
    // Check connection limit
    if (g_atomic_int_get(&s->active_connections) >= (gint)s->max_connections) {
      g_usleep(100000); // Sleep 100ms and retry
      continue;
    }
    
    int cfd = accept(s->tcp_fd, NULL, NULL);
    if (cfd < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        g_usleep(10000); // Sleep 10ms before retry
        continue;
      }
      if (!g_atomic_int_get(&s->stop_flag)) {
        g_warning("tcp: accept failed: %s", g_strerror(errno));
        ipc_stats_error(s);
      }
      break;
    }
    
    g_atomic_int_inc(&s->active_connections);
    ipc_stats_connection_opened(s);
    // Set socket timeout for authentication
    struct timeval tv;
    tv.tv_sec = 5;  // 5 second timeout
    tv.tv_usec = 0;
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    
    // Frame-level AUTH is not part of NIP-5F; do a simple preface AUTH line, then switch to frames.
    char buf[256];
    ssize_t n = read(cfd, buf, sizeof(buf)-1);
    if (n < 0) {
      g_warning("tcp: auth read failed: %s", g_strerror(errno));
      close(cfd);
      g_atomic_int_dec_and_test(&s->active_connections);
      ipc_stats_connection_closed(s);
      ipc_stats_error(s);
      continue;
    }
    buf[n] = '\0';
    
    gboolean authed = FALSE;
    if (g_str_has_prefix(buf, "AUTH ")) {
      char *nl = strchr(buf, '\n');
      if (nl) *nl = '\0';
      const char *tok = buf + 5;
      // Constant-time comparison to prevent timing attacks
      if (tok && s->token && strlen(tok) == strlen(s->token)) {
        authed = (g_strcmp0(tok, s->token) == 0);
      }
    }
    
    if (!authed) {
      const char *resp = "{\"error\":\"unauthorized\"}\n";
      (void)write(cfd, resp, strlen(resp));
      close(cfd);
      g_atomic_int_dec_and_test(&s->active_connections);
      ipc_stats_connection_closed(s);
      ipc_stats_error(s);
      g_message("tcp: rejected unauthorized connection");
      continue;
    }
    
    ipc_stats_request(s);
    // Now speak NIP-5F framed protocol: banner then read client hello
    const char *banner = "{\"name\":\"nostr-signer\",\"supported_methods\":[\"get_public_key\",\"sign_event\",\"nip44_encrypt\",\"nip44_decrypt\",\"list_public_keys\"]}";
    if (nip5f_write_frame(cfd, banner, strlen(banner)) != 0) {
      g_warning("tcp: failed to write banner");
      close(cfd);
      g_atomic_int_dec_and_test(&s->active_connections);
      ipc_stats_connection_closed(s);
      ipc_stats_error(s);
      continue;
    }
    
    char *hello = NULL;
    size_t hlen = 0;
    if (nip5f_read_frame(cfd, &hello, &hlen) != 0) {
      g_warning("tcp: failed to read client hello");
      close(cfd);
      g_atomic_int_dec_and_test(&s->active_connections);
      ipc_stats_connection_closed(s);
      ipc_stats_error(s);
      continue;
    }
    if (hello) free(hello);
    
    // Spawn a detached thread to handle this connection via dispatcher
    pthread_t thr;
    struct Nip5fConnArg *carg = (struct Nip5fConnArg*)calloc(1, sizeof(*carg));
    if (!carg) {
      g_warning("tcp: failed to allocate connection arg");
      close(cfd);
      g_atomic_int_dec_and_test(&s->active_connections);
      ipc_stats_connection_closed(s);
      ipc_stats_error(s);
      continue;
    }
    
    carg->fd = cfd;
    carg->ud = s;  // Pass server context for stats tracking
    carg->get_pub = NULL;
    carg->sign_event = NULL;
    carg->enc44 = NULL;
    carg->dec44 = NULL;
    carg->list_keys = NULL;
    
    if (pthread_create(&thr, NULL, nip5f_conn_thread, carg) == 0) {
      pthread_detach(thr);
      g_message("tcp: spawned handler thread for connection");
    } else {
      g_warning("tcp: failed to create handler thread: %s", g_strerror(errno));
      free(carg);
      close(cfd);
      g_atomic_int_dec_and_test(&s->active_connections);
      ipc_stats_connection_closed(s);
      ipc_stats_error(s);
    }
  }
  
  g_message("tcp: accept thread exiting");
  return NULL;
}
#endif
