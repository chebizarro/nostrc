/* Exercise the production TCP accept/handoff and the real NIP-5F worker.
 * Include the transport to inspect its kernel-assigned endpoint, not to mock it. */
#include "../daemon/ipc.c"

int gnostr_uds_sockd_start(const char *path, GError **error) { (void)path; (void)error; g_assert_not_reached(); }
void gnostr_uds_sockd_stop(void) { g_assert_not_reached(); }

static guint16 unused_port(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  g_assert_cmpint(fd, >=, 0);
  struct sockaddr_in addr = {0}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  g_assert_cmpint(bind(fd, (struct sockaddr *)&addr, sizeof addr), ==, 0);
  socklen_t len = sizeof addr;
  g_assert_cmpint(getsockname(fd, (struct sockaddr *)&addr, &len), ==, 0);
  guint16 port = ntohs(addr.sin_port); close(fd); return port;
}
static int connect_session(GnostrIpcServer *server) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  g_assert_cmpint(fd, >=, 0);
  struct timeval timeout = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
  struct sockaddr_in addr = {0}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(server->port);
  g_assert_cmpint(connect(fd, (struct sockaddr *)&addr, sizeof addr), ==, 0);
  g_autofree gchar *auth = g_strdup_printf("AUTH %s\n", server->token);
  g_assert_cmpint(write(fd, auth, strlen(auth)), ==, strlen(auth));
  char *banner = NULL; size_t len = 0;
  g_assert_cmpint(nip5f_read_frame(fd, &banner, &len), ==, 0);
  g_assert_nonnull(strstr(banner, "supported_methods")); free(banner);
  g_assert_cmpint(nip5f_write_frame(fd, "{}", 2), ==, 0);
  return fd;
}
static void finish_session(int fd) {
  g_assert_cmpint(shutdown(fd, SHUT_WR), ==, 0);
  char byte;
  /* Worker calls the teardown notification before closing its descriptor.
   * EOF is a causal barrier, not a guessed delay or retry. */
  g_assert_cmpint(read(fd, &byte, 1), ==, 0);
  close(fd);
}
static void lifetime(void) {
  g_autofree gchar *endpoint = g_strdup_printf("tcp:127.0.0.1:%u", unused_port());
  GError *error = NULL;
  GnostrIpcServer *server = gnostr_ipc_server_start(endpoint, &error);
  g_assert_no_error(error); g_assert_nonnull(server);
  finish_session(connect_session(server));
  g_assert_cmpint(g_atomic_int_get(&server->active_connections), ==, 0);
  finish_session(connect_session(server)); /* max_connections=1 must admit again */
  g_assert_cmpint(g_atomic_int_get(&server->active_connections), ==, 0);
  int live = connect_session(server);
  gnostr_ipc_server_stop(server); /* connection owns accounting until teardown */
  finish_session(live);
  while (g_main_context_iteration(NULL, FALSE)) {}
  g_assert_cmpuint(g_ipc_stats.connections_active, ==, 0);
}
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_autofree gchar *runtime = g_dir_make_tmp("grotto-tcp-XXXXXX", NULL);
  g_setenv("XDG_RUNTIME_DIR", runtime, TRUE);
  g_setenv("NOSTR_SIGNER_MAX_CONNECTIONS", "1", TRUE);
  g_test_add_func("/ipc/tcp/close-accounting-lifetime", lifetime);
  int result = g_test_run();
  g_autofree gchar *token = g_build_filename(runtime, "gnostr", "token", NULL);
  g_autofree gchar *dir = g_build_filename(runtime, "gnostr", NULL);
  g_unlink(token); g_rmdir(dir); g_rmdir(runtime);
  return result;
}
