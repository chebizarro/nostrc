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

/* Windows Named Pipe IPC Support */
#ifdef G_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <stddef.h>
#include "nostr/nip5f/nip5f.h"
#include "json.h"

/* Connection argument for Windows named pipe handler thread */
typedef struct NpipeConnArg {
  HANDLE pipe;
  void *ud;
  Nip5fGetPubFn get_pub;
  Nip5fSignEventFn sign_event;
  Nip5fNip44EncFn enc44;
  Nip5fNip44DecFn dec44;
  Nip5fListKeysFn list_keys;
  gpointer server;  /* Back pointer to GnostrIpcServer for stats */
} NpipeConnArg;

/* Forward declarations for Windows named pipe functions */
static gpointer npipe_ipc_accept_thread(gpointer data);
static gpointer npipe_conn_handler_thread(gpointer data);
static int npipe_read_frame(HANDLE pipe, char **out_json, size_t *out_len);
static int npipe_write_frame(HANDLE pipe, const char *json, size_t len);
#endif /* G_OS_WIN32 */

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
#ifdef G_OS_WIN32
  HANDLE npipe_handle;   /* Current pipe instance for accept */
  GThread *npipe_thr;    /* Accept thread */
  gint npipe_stop_flag;  /* Stop signal for thread */
  gchar *npipe_token_path;
  gchar *npipe_token;
  guint npipe_max_connections;
  gint npipe_active_connections;
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
      GError *write_err = NULL;
      if (!g_file_set_contents(srv->token_path, hex, 64, &write_err)) {
        g_warning("tcp: failed to write token file %s: %s",
                  srv->token_path, write_err ? write_err->message : "unknown");
        g_clear_error(&write_err);
      }
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
    /* Parse npipe:\\.\pipe\name format */
    const char *pipe_name = ep + 6;  /* Skip "npipe:" prefix */

    /* Validate pipe name format */
    if (!g_str_has_prefix(pipe_name, "\\\\.\\pipe\\") &&
        !g_str_has_prefix(pipe_name, "\\\\\\\\.\\\\pipe\\\\")) {
      g_warning("npipe: invalid pipe name format: %s (expected \\\\.\\pipe\\name)", pipe_name);
      if (ep_alloc) g_free(ep_alloc);
      ipc_stats_cleanup(srv);
      g_free((gpointer)srv);
      return NULL;
    }

    /* Normalize escaped backslashes if present (from command line) */
    gchar *normalized_name = g_strdup(pipe_name);
    if (g_str_has_prefix(pipe_name, "\\\\\\\\.\\\\pipe\\\\")) {
      g_free(normalized_name);
      /* Convert \\\\.\\ to \\.\ and \\\\pipe\\ to \\pipe\ */
      normalized_name = g_strdup_printf("\\\\.\\pipe\\%s",
                                         pipe_name + strlen("\\\\\\\\.\\\\pipe\\\\"));
    }

    /* Set maximum concurrent connections */
    const char *max_conn_env = g_getenv("NOSTR_SIGNER_MAX_CONNECTIONS");
    srv->npipe_max_connections = max_conn_env ? (guint)g_ascii_strtoull(max_conn_env, NULL, 10) : 100;
    if (srv->npipe_max_connections == 0) srv->npipe_max_connections = 100;
    srv->npipe_active_connections = 0;

    /* Prepare token file under LOCALAPPDATA/gnostr/token for authentication */
    const char *localapp = g_getenv("LOCALAPPDATA");
    if (!localapp || !*localapp) {
      localapp = g_get_user_config_dir();
    }
    gchar *dir = g_build_filename(localapp, "gnostr", NULL);
    g_mkdir_with_parents(dir, 0700);
    srv->npipe_token_path = g_build_filename(dir, "npipe-token", NULL);
    g_free(dir);

    /* Load or create authentication token (64 hex chars) */
    GError *err = NULL;
    if (!g_file_get_contents(srv->npipe_token_path, &srv->npipe_token, NULL, &err)) {
      if (err) g_clear_error(&err);
      guint8 rnd[32];
      g_random_set_seed((guint32)g_get_monotonic_time());
      for (gsize i = 0; i < sizeof(rnd); ++i) {
        rnd[i] = (guint8)g_random_int_range(0, 256);
      }
      gchar *hex = g_malloc0(65);
      static const char *H = "0123456789abcdef";
      for (int i = 0; i < 32; i++) {
        hex[i * 2] = H[(rnd[i] >> 4) & 0xF];
        hex[i * 2 + 1] = H[rnd[i] & 0xF];
      }
      GError *write_err = NULL;
      if (!g_file_set_contents(srv->npipe_token_path, hex, 64, &write_err)) {
        g_warning("npipe: failed to write token file %s: %s",
                  srv->npipe_token_path, write_err ? write_err->message : "unknown");
        g_clear_error(&write_err);
      }
      srv->npipe_token = hex;
    }

    /* Store endpoint for accept thread */
    srv->endpoint = normalized_name;

    /* Start accept thread */
    g_atomic_int_set(&srv->npipe_stop_flag, 0);
    srv->npipe_thr = g_thread_new("gnostr-npipe-ipc", npipe_ipc_accept_thread, srv);
    if (!srv->npipe_thr) {
      g_warning("npipe: failed to start accept thread");
      g_free(srv->endpoint);
      g_free(srv->npipe_token_path);
      g_free(srv->npipe_token);
      if (ep_alloc) g_free(ep_alloc);
      ipc_stats_cleanup(srv);
      g_free((gpointer)srv);
      return NULL;
    }

    srv->kind = IPC_NPIPE;
    if (ep_alloc) g_free(ep_alloc);
    g_message("npipe ipc server started on %s (token: %s, max_connections: %u)",
              srv->endpoint, srv->npipe_token_path, srv->npipe_max_connections);
    return srv;
#else
    g_warning("npipe: endpoint only supported on Windows");
    if (ep_alloc) g_free(ep_alloc);
    ipc_stats_cleanup(srv);
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

#ifdef G_OS_WIN32
  if (srv->kind == IPC_NPIPE) {
    /* Signal stop and wait for accept thread */
    g_atomic_int_set(&srv->npipe_stop_flag, 1);

    /* Cancel any pending ConnectNamedPipe by creating a dummy connection */
    if (srv->endpoint) {
      HANDLE dummy = CreateFileA(
          srv->endpoint,
          GENERIC_READ | GENERIC_WRITE,
          0,
          NULL,
          OPEN_EXISTING,
          0,
          NULL);
      if (dummy != INVALID_HANDLE_VALUE) {
        CloseHandle(dummy);
      }
    }

    if (srv->npipe_thr) {
      g_message("npipe: waiting for accept thread to finish");
      g_thread_join(srv->npipe_thr);
      srv->npipe_thr = NULL;
    }

    g_clear_pointer(&srv->npipe_token_path, g_free);
    g_clear_pointer(&srv->npipe_token, g_free);
    g_message("npipe: server stopped");
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

/* Windows Named Pipe IPC Implementation */
#ifdef G_OS_WIN32
/* Maximum frame size for NIP-5F protocol */
#define NPIPE_MAX_FRAME (1024 * 1024)
#define NPIPE_BUFFER_SIZE 4096

/* Read a length-prefixed frame from named pipe.
 * Protocol: 4-byte big-endian length followed by JSON payload.
 * Returns 0 on success, -1 on error. */
static int npipe_read_frame(HANDLE pipe, char **out_json, size_t *out_len) {
  if (!out_json || !out_len) return -1;
  *out_json = NULL;
  *out_len = 0;

  /* Read 4-byte length prefix */
  guint8 hdr[4];
  DWORD bytes_read = 0;
  BOOL ok = ReadFile(pipe, hdr, 4, &bytes_read, NULL);
  if (!ok || bytes_read != 4) {
    return -1;
  }

  /* Parse big-endian length */
  guint32 frame_len = ((guint32)hdr[0] << 24) | ((guint32)hdr[1] << 16) |
                      ((guint32)hdr[2] << 8)  | (guint32)hdr[3];

  if (frame_len == 0 || frame_len > NPIPE_MAX_FRAME) {
    g_warning("npipe: invalid frame length %u", frame_len);
    return -1;
  }

  /* Allocate buffer and read payload */
  char *buf = (char*)g_malloc(frame_len + 1);
  if (!buf) return -1;

  DWORD total_read = 0;
  while (total_read < frame_len) {
    DWORD to_read = frame_len - total_read;
    DWORD n = 0;
    ok = ReadFile(pipe, buf + total_read, to_read, &n, NULL);
    if (!ok || n == 0) {
      g_free(buf);
      return -1;
    }
    total_read += n;
  }
  buf[frame_len] = '\0';

  *out_json = buf;
  *out_len = frame_len;
  return 0;
}

/* Write a length-prefixed frame to named pipe.
 * Returns 0 on success, -1 on error. */
static int npipe_write_frame(HANDLE pipe, const char *json, size_t len) {
  if (!json || len == 0 || len > NPIPE_MAX_FRAME) return -1;

  /* Build 4-byte big-endian length header */
  guint8 hdr[4];
  hdr[0] = (guint8)((len >> 24) & 0xFF);
  hdr[1] = (guint8)((len >> 16) & 0xFF);
  hdr[2] = (guint8)((len >> 8) & 0xFF);
  hdr[3] = (guint8)(len & 0xFF);

  /* Write header */
  DWORD bytes_written = 0;
  BOOL ok = WriteFile(pipe, hdr, 4, &bytes_written, NULL);
  if (!ok || bytes_written != 4) {
    return -1;
  }

  /* Write payload */
  DWORD total_written = 0;
  while (total_written < len) {
    DWORD to_write = (DWORD)(len - total_written);
    DWORD n = 0;
    ok = WriteFile(pipe, json + total_written, to_write, &n, NULL);
    if (!ok || n == 0) {
      return -1;
    }
    total_written += n;
  }

  FlushFileBuffers(pipe);
  return 0;
}

/* Build a minimal error response JSON */
static char *npipe_build_error_json(const char *id, int code, const char *msg) {
  const char *id_field = id ? id : "";
  size_t need = strlen(id_field) + strlen(msg) + 96;
  char *buf = (char*)g_malloc(need);
  if (!buf) return NULL;
  g_snprintf(buf, need, "{\"id\":\"%s\",\"result\":null,\"error\":{\"code\":%d,\"message\":\"%s\"}}",
             id_field, code, msg);
  return buf;
}

/* Build a minimal success response with raw JSON result */
static char *npipe_build_ok_json_raw(const char *id, const char *raw_json) {
  const char *id_field = id ? id : "";
  size_t need = strlen(id_field) + strlen(raw_json) + 64;
  char *buf = (char*)g_malloc(need);
  if (!buf) return NULL;
  g_snprintf(buf, need, "{\"id\":\"%s\",\"result\":%s,\"error\":null}", id_field, raw_json);
  return buf;
}

/* Connection handler thread for Windows named pipe.
 * Similar to nip5f_conn_thread but uses Windows pipe I/O. */
static gpointer npipe_conn_handler_thread(gpointer data) {
  NpipeConnArg *carg = (NpipeConnArg*)data;
  HANDLE pipe = carg->pipe;
  GnostrIpcServer *srv = (GnostrIpcServer*)carg->server;

  g_message("npipe: client connected");

  /* Process requests in a loop */
  for (;;) {
    char *req = NULL;
    size_t rlen = 0;
    if (npipe_read_frame(pipe, &req, &rlen) != 0) {
      break;
    }

    /* Extract id and method from request */
    char *id = NULL;
    char *method = NULL;
    (void)nostr_json_get_string(req, "id", &id);
    (void)nostr_json_get_string(req, "method", &method);

    if (!method) {
      char *err = npipe_build_error_json(id, 1, "invalid request");
      if (err) {
        npipe_write_frame(pipe, err, strlen(err));
        g_free(err);
      }
      g_free(id);
      g_free(req);
      continue;
    }

    /* Dispatch methods - same as NIP-5F protocol */
    if (g_strcmp0(method, "get_public_key") == 0) {
      char *pub = NULL;
      int rc = -2;
      if (carg->get_pub) rc = carg->get_pub(carg->ud, &pub);
      else rc = nostr_nip5f_builtin_get_public_key(&pub);

      if (rc == 0 && pub) {
        size_t L = strlen(pub) + 3;
        char *jres = (char*)g_malloc(L);
        if (jres) {
          g_snprintf(jres, L, "\"%s\"", pub);
          char *ok = npipe_build_ok_json_raw(id, jres);
          if (ok) {
            npipe_write_frame(pipe, ok, strlen(ok));
            g_free(ok);
          }
          g_free(jres);
        }
        g_free(pub);
      } else {
        char *err = npipe_build_error_json(id, 10, "get_public_key failed");
        if (err) {
          npipe_write_frame(pipe, err, strlen(err));
          g_free(err);
        }
      }
    } else if (g_strcmp0(method, "sign_event") == 0) {
      char *ev = NULL;
      char *pub = NULL;
      (void)nostr_json_get_string_at(req, "params", "event", &ev);
      (void)nostr_json_get_string_at(req, "params", "pubkey", &pub);

      if (!ev) {
        char *err = npipe_build_error_json(id, 1, "invalid params");
        if (err) {
          npipe_write_frame(pipe, err, strlen(err));
          g_free(err);
        }
      } else {
        char *signed_json = NULL;
        int rc = -2;
        if (carg->sign_event) rc = carg->sign_event(carg->ud, ev, pub, &signed_json);
        else rc = nostr_nip5f_builtin_sign_event(ev, pub, &signed_json);

        if (rc == 0 && signed_json) {
          char *ok = npipe_build_ok_json_raw(id, signed_json);
          if (ok) {
            npipe_write_frame(pipe, ok, strlen(ok));
            g_free(ok);
          }
          g_free(signed_json);
        } else {
          char *err = npipe_build_error_json(id, 10, "sign_event failed");
          if (err) {
            npipe_write_frame(pipe, err, strlen(err));
            g_free(err);
          }
        }
      }
      g_free(ev);
      g_free(pub);
    } else if (g_strcmp0(method, "nip44_encrypt") == 0) {
      char *peer = NULL;
      char *pt = NULL;
      (void)nostr_json_get_string_at(req, "params", "peer_pub", &peer);
      (void)nostr_json_get_string_at(req, "params", "plaintext", &pt);

      if (!peer || !pt) {
        char *err = npipe_build_error_json(id, 1, "invalid params");
        if (err) {
          npipe_write_frame(pipe, err, strlen(err));
          g_free(err);
        }
      } else {
        char *b64 = NULL;
        int rc = -2;
        if (carg->enc44) rc = carg->enc44(carg->ud, peer, pt, &b64);
        else rc = nostr_nip5f_builtin_nip44_encrypt(peer, pt, &b64);

        if (rc == 0 && b64) {
          size_t L = strlen(b64) + 3;
          char *jres = (char*)g_malloc(L);
          if (jres) {
            g_snprintf(jres, L, "\"%s\"", b64);
            char *ok = npipe_build_ok_json_raw(id, jres);
            if (ok) {
              npipe_write_frame(pipe, ok, strlen(ok));
              g_free(ok);
            }
            g_free(jres);
          }
          g_free(b64);
        } else {
          char *err = npipe_build_error_json(id, 10, "nip44_encrypt failed");
          if (err) {
            npipe_write_frame(pipe, err, strlen(err));
            g_free(err);
          }
        }
      }
      g_free(peer);
      g_free(pt);
    } else if (g_strcmp0(method, "nip44_decrypt") == 0) {
      char *peer = NULL;
      char *ct = NULL;
      (void)nostr_json_get_string_at(req, "params", "peer_pub", &peer);
      (void)nostr_json_get_string_at(req, "params", "cipher_b64", &ct);

      if (!peer || !ct) {
        char *err = npipe_build_error_json(id, 1, "invalid params");
        if (err) {
          npipe_write_frame(pipe, err, strlen(err));
          g_free(err);
        }
      } else {
        char *plaintext = NULL;
        int rc = -2;
        if (carg->dec44) rc = carg->dec44(carg->ud, peer, ct, &plaintext);
        else rc = nostr_nip5f_builtin_nip44_decrypt(peer, ct, &plaintext);

        if (rc == 0 && plaintext) {
          size_t L = strlen(plaintext) + 3;
          char *jres = (char*)g_malloc(L);
          if (jres) {
            g_snprintf(jres, L, "\"%s\"", plaintext);
            char *ok = npipe_build_ok_json_raw(id, jres);
            if (ok) {
              npipe_write_frame(pipe, ok, strlen(ok));
              g_free(ok);
            }
            g_free(jres);
          }
          g_free(plaintext);
        } else {
          char *err = npipe_build_error_json(id, 10, "nip44_decrypt failed");
          if (err) {
            npipe_write_frame(pipe, err, strlen(err));
            g_free(err);
          }
        }
      }
      g_free(peer);
      g_free(ct);
    } else if (g_strcmp0(method, "list_public_keys") == 0) {
      char *arr = NULL;
      int rc = -2;
      if (carg->list_keys) rc = carg->list_keys(carg->ud, &arr);
      else rc = nostr_nip5f_builtin_list_public_keys(&arr);

      if (rc == 0 && arr) {
        char *ok = npipe_build_ok_json_raw(id, arr);
        if (ok) {
          npipe_write_frame(pipe, ok, strlen(ok));
          g_free(ok);
        }
        g_free(arr);
      } else {
        char *err = npipe_build_error_json(id, 10, "list_public_keys failed");
        if (err) {
          npipe_write_frame(pipe, err, strlen(err));
          g_free(err);
        }
      }
    } else if (g_strcmp0(method, "activate_uri") == 0) {
      /* Single-instance URI activation support */
      char *uri = NULL;
      (void)nostr_json_get_string_at(req, "params", "uri", &uri);

      if (uri) {
        g_message("npipe: received URI activation: %s", uri);
        /* In a real implementation, this would signal the main window
         * to handle the nostr: URI. For now, acknowledge receipt. */
        char *ok = npipe_build_ok_json_raw(id, "true");
        if (ok) {
          npipe_write_frame(pipe, ok, strlen(ok));
          g_free(ok);
        }
        g_free(uri);
      } else {
        char *err = npipe_build_error_json(id, 1, "invalid params");
        if (err) {
          npipe_write_frame(pipe, err, strlen(err));
          g_free(err);
        }
      }
    } else {
      char *err = npipe_build_error_json(id, 2, "method not supported");
      if (err) {
        npipe_write_frame(pipe, err, strlen(err));
        g_free(err);
      }
    }

    g_free(id);
    g_free(method);
    g_free(req);
  }

  g_message("npipe: client disconnected");

  /* Clean up */
  FlushFileBuffers(pipe);
  DisconnectNamedPipe(pipe);
  CloseHandle(pipe);

  /* Update stats */
  if (srv) {
    g_atomic_int_dec_and_test(&srv->npipe_active_connections);
    ipc_stats_connection_closed(srv);
  }

  g_free(carg);
  return NULL;
}

/* Create a security descriptor for the named pipe.
 * Restricts access to the current user only for security. */
static PSECURITY_DESCRIPTOR npipe_create_security_descriptor(void) {
  PSECURITY_DESCRIPTOR pSD = NULL;

  /* SDDL string: D:P(A;;GA;;;CO) - Allow Generic All to Creator Owner only */
  /* This ensures only the current user can access the pipe */
  const char *sddl = "D:P(A;;GA;;;CO)(A;;GA;;;BA)";

  if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
          sddl, SDDL_REVISION_1, &pSD, NULL)) {
    g_warning("npipe: failed to create security descriptor: %lu", GetLastError());
    return NULL;
  }

  return pSD;
}

/* Accept thread for Windows named pipe server.
 * Creates pipe instances and waits for client connections. */
static gpointer npipe_ipc_accept_thread(gpointer data) {
  GnostrIpcServer *s = (GnostrIpcServer*)data;

  /* Ensure JSON interface is initialized */
  extern NostrJsonInterface *jansson_impl;
  nostr_set_json_interface(jansson_impl);
  nostr_json_init();

  g_message("npipe: accept thread started on %s", s->endpoint);

  /* Create security attributes for the pipe */
  PSECURITY_DESCRIPTOR pSD = npipe_create_security_descriptor();
  SECURITY_ATTRIBUTES sa;
  sa.nLength = sizeof(SECURITY_ATTRIBUTES);
  sa.lpSecurityDescriptor = pSD;
  sa.bInheritHandle = FALSE;

  while (!g_atomic_int_get(&s->npipe_stop_flag)) {
    /* Check connection limit */
    if (g_atomic_int_get(&s->npipe_active_connections) >= (gint)s->npipe_max_connections) {
      Sleep(100);
      continue;
    }

    /* Create a new pipe instance for this connection */
    HANDLE pipe = CreateNamedPipeA(
        s->endpoint,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        NPIPE_BUFFER_SIZE,
        NPIPE_BUFFER_SIZE,
        0,
        pSD ? &sa : NULL);

    if (pipe == INVALID_HANDLE_VALUE) {
      DWORD err = GetLastError();
      if (!g_atomic_int_get(&s->npipe_stop_flag)) {
        g_warning("npipe: CreateNamedPipe failed: %lu", err);
        ipc_stats_error(s);
      }
      Sleep(100);
      continue;
    }

    /* Wait for a client to connect */
    BOOL connected = ConnectNamedPipe(pipe, NULL);
    if (!connected) {
      DWORD err = GetLastError();
      if (err == ERROR_PIPE_CONNECTED) {
        /* Client already connected before we called ConnectNamedPipe */
        connected = TRUE;
      } else if (!g_atomic_int_get(&s->npipe_stop_flag)) {
        g_warning("npipe: ConnectNamedPipe failed: %lu", err);
        CloseHandle(pipe);
        ipc_stats_error(s);
        continue;
      } else {
        CloseHandle(pipe);
        break;
      }
    }

    g_atomic_int_inc(&s->npipe_active_connections);
    ipc_stats_connection_opened(s);

    /* Perform token authentication if enabled */
    gboolean authed = FALSE;
    if (s->npipe_token && s->npipe_token[0]) {
      /* Read AUTH line: "AUTH <token>\n" */
      char buf[256];
      DWORD bytes_read = 0;
      if (ReadFile(pipe, buf, sizeof(buf) - 1, &bytes_read, NULL) && bytes_read > 0) {
        buf[bytes_read] = '\0';
        if (g_str_has_prefix(buf, "AUTH ")) {
          char *nl = strchr(buf, '\n');
          if (nl) *nl = '\0';
          const char *tok = buf + 5;
          if (tok && strlen(tok) == strlen(s->npipe_token) &&
              g_strcmp0(tok, s->npipe_token) == 0) {
            authed = TRUE;
          }
        }
      }

      if (!authed) {
        const char *resp = "{\"error\":\"unauthorized\"}\n";
        DWORD written = 0;
        WriteFile(pipe, resp, (DWORD)strlen(resp), &written, NULL);
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        g_atomic_int_dec_and_test(&s->npipe_active_connections);
        ipc_stats_connection_closed(s);
        ipc_stats_error(s);
        g_message("npipe: rejected unauthorized connection");
        continue;
      }
    } else {
      /* No token auth required - proceed directly */
      authed = TRUE;
    }

    /* Send NIP-5F banner */
    const char *banner = "{\"name\":\"nostr-signer\",\"supported_methods\":[\"get_public_key\",\"sign_event\",\"nip44_encrypt\",\"nip44_decrypt\",\"list_public_keys\",\"activate_uri\"]}";
    if (npipe_write_frame(pipe, banner, strlen(banner)) != 0) {
      g_warning("npipe: failed to write banner");
      DisconnectNamedPipe(pipe);
      CloseHandle(pipe);
      g_atomic_int_dec_and_test(&s->npipe_active_connections);
      ipc_stats_connection_closed(s);
      ipc_stats_error(s);
      continue;
    }

    /* Read client hello (ignore content for now) */
    char *hello = NULL;
    size_t hlen = 0;
    if (npipe_read_frame(pipe, &hello, &hlen) != 0) {
      g_warning("npipe: failed to read client hello");
      DisconnectNamedPipe(pipe);
      CloseHandle(pipe);
      g_atomic_int_dec_and_test(&s->npipe_active_connections);
      ipc_stats_connection_closed(s);
      ipc_stats_error(s);
      continue;
    }
    g_free(hello);

    /* Spawn handler thread for this connection */
    NpipeConnArg *carg = (NpipeConnArg*)g_new0(NpipeConnArg, 1);
    if (!carg) {
      g_warning("npipe: failed to allocate connection arg");
      DisconnectNamedPipe(pipe);
      CloseHandle(pipe);
      g_atomic_int_dec_and_test(&s->npipe_active_connections);
      ipc_stats_connection_closed(s);
      ipc_stats_error(s);
      continue;
    }

    carg->pipe = pipe;
    carg->server = s;
    carg->ud = NULL;
    carg->get_pub = NULL;
    carg->sign_event = NULL;
    carg->enc44 = NULL;
    carg->dec44 = NULL;
    carg->list_keys = NULL;

    GThread *handler = g_thread_new("npipe-conn", npipe_conn_handler_thread, carg);
    if (handler) {
      g_thread_unref(handler);  /* Detach thread */
      g_message("npipe: spawned handler thread for connection");
    } else {
      g_warning("npipe: failed to create handler thread");
      g_free(carg);
      DisconnectNamedPipe(pipe);
      CloseHandle(pipe);
      g_atomic_int_dec_and_test(&s->npipe_active_connections);
      ipc_stats_connection_closed(s);
      ipc_stats_error(s);
    }
  }

  /* Cleanup */
  if (pSD) {
    LocalFree(pSD);
  }

  g_message("npipe: accept thread exiting");
  return NULL;
}
#endif /* G_OS_WIN32 */
