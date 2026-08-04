/* SPDX-License-Identifier: MIT */

#include "signet/health_server.h"
#include "signet/bootstrap_server.h"
#include "signet/nip5l_transport.h"
#include "signet/signet_config.h"
#include "signet/ssh_agent.h"
#include "signet/audit_logger.h"
#include "signet/key_store.h"
#include "signet/store.h"
#include "signet/store_secrets.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <glib.h>
#include <sodium.h>

#define FLOOD_CONNECTIONS 24

static int connect_unix_socket(const char *path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  g_strlcpy(addr.sun_path, path, sizeof(addr.sun_path));
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static bool wait_for_counter(volatile gint *counter, gint before) {
  for (int i = 0; i < 500; i++) {
    if (g_atomic_int_get(counter) > before) return true;
    g_usleep(10000);
  }
  return false;
}

static char *resolve_test_uid(uid_t uid, void *user_data) {
  (void)uid;
  (void)user_data;
  return g_strdup("load-agent");
}

static gpointer run_main_loop(gpointer data) {
  g_main_loop_run((GMainLoop *)data);
  return NULL;
}

static void test_ssh_connection_flood(void) {
  char *path = g_strdup_printf("/tmp/signet-item3-ssh-%u.sock", g_random_int());
  SignetSshAgentConfig cfg = {
    .socket_path = path,
    .keys = (struct SignetKeyStore *)1,
    .uid_resolver = resolve_test_uid,
  };
  SignetSshAgent *server = signet_ssh_agent_new(&cfg);
  assert(server != NULL);
  assert(signet_ssh_agent_start(server) == 0);

  gint denied_before =
      g_atomic_int_get(&g_signet_metrics.ssh_agent_connections_denied);
  int fds[FLOOD_CONNECTIONS];
  for (int i = 0; i < FLOOD_CONNECTIONS; i++)
    fds[i] = connect_unix_socket(path);

  assert(wait_for_counter(&g_signet_metrics.ssh_agent_connections_denied,
                          denied_before));

  /* stop() must close idle clients and wait for every detached worker. */
  signet_ssh_agent_stop(server);
  signet_ssh_agent_free(server);
  for (int i = 0; i < FLOOD_CONNECTIONS; i++)
    if (fds[i] >= 0) close(fds[i]);
  g_free(path);
}

static void test_nip5l_connection_flood(void) {
  char *path = g_strdup_printf("/tmp/signet-item3-nip5l-%u.sock", g_random_int());
  SignetNip5lServerConfig cfg = {
    .socket_path = path,
    .keys = (struct SignetKeyStore *)1,
    .challenges = (struct SignetChallengeStore *)1,
  };
  SignetNip5lServer *server = signet_nip5l_server_new(&cfg);
  assert(server != NULL);
  assert(signet_nip5l_server_start(server) == 0);

  GMainLoop *loop = g_main_loop_new(NULL, FALSE);
  GThread *loop_thread = g_thread_new("item3-main-loop", run_main_loop, loop);

  gint denied_before =
      g_atomic_int_get(&g_signet_metrics.nip5l_connections_denied);
  int fds[FLOOD_CONNECTIONS];
  for (int i = 0; i < FLOOD_CONNECTIONS; i++)
    fds[i] = connect_unix_socket(path);

  assert(wait_for_counter(&g_signet_metrics.nip5l_connections_denied,
                          denied_before));

  signet_nip5l_server_stop(server);
  signet_nip5l_server_free(server);
  g_main_loop_quit(loop);
  g_thread_join(loop_thread);
  g_main_loop_unref(loop);
  for (int i = 0; i < FLOOD_CONNECTIONS; i++)
    if (fds[i] >= 0) close(fds[i]);
  g_free(path);
}

static void assert_invalid_port_env(const char *name, const char *value) {
  g_setenv(name, value, TRUE);
  SignetConfig cfg;
  assert(signet_config_load(NULL, &cfg) == 0);
  char err[128] = {0};
  assert(signet_config_validate(&cfg, err, sizeof(err)) == -1);
  assert(strstr(err, "port") != NULL);
  signet_config_clear(&cfg);
  g_unsetenv(name);
}

#define STORE_LOAD_THREADS 4
#define STORE_LOAD_ITERATIONS 1500
#define LOAD_MASTER_KEY "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

typedef struct {
  SignetKeyStore *keys;
  SignetStore *store;
  volatile gint failures;
} StoreLoadContext;

static gpointer signing_load_worker(gpointer data) {
  StoreLoadContext *ctx = data;
  for (int i = 0; i < STORE_LOAD_ITERATIONS; i++) {
    SignetLoadedKey key;
    memset(&key, 0, sizeof(key));
    /* This is the NIP-46 signing hot path: load the custody key, which also
     * persists agents.last_used on the shared SQLCipher connection. */
    if (!signet_key_store_load_agent_key(ctx->keys, "load-agent", &key)) {
      g_atomic_int_inc(&ctx->failures);
      continue;
    }
    signet_loaded_key_clear(&key);
  }
  return NULL;
}

static gpointer credential_load_worker(gpointer data) {
  StoreLoadContext *ctx = data;
  static const char expected[] = "concurrent-credential-value";
  for (int i = 0; i < STORE_LOAD_ITERATIONS; i++) {
    SignetSecretRecord rec;
    memset(&rec, 0, sizeof(rec));
    if (signet_store_get_secret(ctx->store, "load-credential", &rec) != 0 ||
        rec.payload_len != sizeof(expected) - 1 ||
        memcmp(rec.payload, expected, sizeof(expected) - 1) != 0) {
      g_atomic_int_inc(&ctx->failures);
    }
    signet_secret_record_clear(&rec);
  }
  return NULL;
}

static void test_concurrent_signing_and_credential_retrieval(void) {
  char path[] = "/tmp/signet-item3-store-load-XXXXXX.db";
  int fd = mkstemps(path, 3);
  assert(fd >= 0);
  close(fd);
  unlink(path);

  SignetAuditLoggerConfig audit_cfg = {
    .path = NULL,
    .to_stdout = false,
    .flush_each_write = false,
  };
  SignetAuditLogger *audit = signet_audit_logger_new(&audit_cfg);
  assert(audit != NULL);
  SignetKeyStoreConfig key_cfg = {
    .db_path = path,
    .master_key = LOAD_MASTER_KEY,
  };
  SignetKeyStore *keys = signet_key_store_new(audit, &key_cfg);
  assert(keys != NULL);

  uint8_t agent_secret[32];
  randombytes_buf(agent_secret, sizeof(agent_secret));
  char agent_pubkey[65] = {0};
  assert(signet_key_store_adopt_agent(
      keys, "load-agent", agent_secret, NULL, NULL,
      NULL, NULL, 0, agent_pubkey, NULL) == SIGNET_ADOPT_OK);
  sodium_memzero(agent_secret, sizeof(agent_secret));

  SignetStore *store = signet_key_store_get_store(keys);
  assert(store != NULL);
  static const char credential[] = "concurrent-credential-value";
  assert(signet_store_put_secret(
      store, "load-credential", "load-agent", agent_pubkey,
      SIGNET_SECRET_API_TOKEN, "load credential",
      (const uint8_t *)credential, sizeof(credential) - 1,
      NULL, (int64_t)time(NULL)) == 0);

  StoreLoadContext ctx = {
    .keys = keys,
    .store = store,
    .failures = 0,
  };
  GThread *signers[STORE_LOAD_THREADS];
  GThread *readers[STORE_LOAD_THREADS];
  for (int i = 0; i < STORE_LOAD_THREADS; i++) {
    signers[i] = g_thread_new("nip46-sign-load", signing_load_worker, &ctx);
    readers[i] = g_thread_new("credential-load", credential_load_worker, &ctx);
  }
  for (int i = 0; i < STORE_LOAD_THREADS; i++) {
    g_thread_join(signers[i]);
    g_thread_join(readers[i]);
  }
  assert(g_atomic_int_get(&ctx.failures) == 0);

  signet_key_store_free(keys);
  signet_audit_logger_free(audit);
  unlink(path);
}

static void test_checked_port_parsing(void) {
  assert_invalid_port_env("SIGNET_HEALTH_PORT", "65536");
  assert_invalid_port_env("SIGNET_BOOTSTRAP_PORT", "-1");
  assert_invalid_port_env("SIGNET_DBUS_TCP_PORT", "not-a-port");

  g_setenv("SIGNET_HEALTH_PORT", "0", TRUE);
  SignetConfig cfg;
  assert(signet_config_load(NULL, &cfg) == 0);
  assert(cfg.health_port == 0);
  signet_config_clear(&cfg);
  g_unsetenv("SIGNET_HEALTH_PORT");

  const char *bad_listens[] = {
    "127.0.0.1:65536",
    "127.0.0.1:-1",
    "127.0.0.1:not-a-port",
  };
  for (size_t i = 0; i < G_N_ELEMENTS(bad_listens); i++) {
    SignetBootstrapServerConfig bootstrap_cfg = {
      .listen = bad_listens[i],
      .keys = (struct SignetKeyStore *)1,
      .store = (struct SignetStore *)1,
      .challenges = (struct SignetChallengeStore *)1,
    };
    SignetBootstrapServer *bootstrap = signet_bootstrap_server_new(&bootstrap_cfg);
    assert(bootstrap != NULL);
    assert(signet_bootstrap_server_start(bootstrap) == -1);
    signet_bootstrap_server_free(bootstrap);
  }
}

int main(void) {
  assert(sodium_init() >= 0);
  test_checked_port_parsing();
  test_concurrent_signing_and_credential_retrieval();
  test_ssh_connection_flood();
  test_nip5l_connection_flood();
  printf("Item 3 connection and port robustness tests passed.\n");
  return 0;
}
