/* SPDX-License-Identifier: MIT
 *
 * test_audit_logger.c - Audit logger tests
 */

#include "signet/audit_logger.h"

#include "test_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static void test_audit_logger_basic(void) {
  /* Create logger to stdout */
  SignetAuditLoggerConfig cfg = {
    .path = NULL,
    .to_stdout = true,
    .flush_each_write = true,
  };

  SignetAuditLogger *logger = signet_audit_logger_new(&cfg);
  CHECK(logger != NULL);

  /* Write a basic audit entry */
  int rc = signet_audit_log_json(logger, SIGNET_AUDIT_EVENT_STARTUP, "{\"test\":\"message\"}");
  CHECK(rc == 0);

  signet_audit_logger_free(logger);
  printf("test_audit_logger_basic: PASS\n");
}

static void test_audit_logger_file(void) {
  char directory[] = "/tmp/signet-audit-XXXXXX";
  CHECK(mkdtemp(directory));
  char test_path[256], link_path[256];
  snprintf(test_path, sizeof(test_path), "%s/audit", directory);
  snprintf(link_path, sizeof(link_path), "%s/link", directory);
  FILE *existing = fopen(test_path, "w"); CHECK(existing);
  CHECK(fclose(existing) == 0);
  CHECK(chmod(test_path, 0644) == 0);

  SignetAuditLoggerConfig cfg = {
    .path = test_path,
    .to_stdout = false,
    .flush_each_write = true,
  };

  SignetAuditLogger *logger = signet_audit_logger_new(&cfg);
  CHECK(logger != NULL);
  struct stat st;
  CHECK(stat(test_path, &st) == 0 && (st.st_mode & 0777) == 0600);
  CHECK(symlink(test_path, link_path) == 0);
  SignetAuditLoggerConfig unsafe = cfg; unsafe.path = link_path;
  CHECK(signet_audit_logger_new(&unsafe) == NULL);
  CHECK(unlink(link_path) == 0);
  CHECK(mkfifo(link_path, 0600) == 0);
  CHECK(signet_audit_logger_new(&unsafe) == NULL);
  CHECK(unlink(link_path) == 0);

  /* Write multiple entries */
  signet_audit_log_json(logger, SIGNET_AUDIT_EVENT_STARTUP, "{\"event\":\"1\"}");
  signet_audit_log_json(logger, SIGNET_AUDIT_EVENT_SHUTDOWN, "{\"event\":\"2\"}");

  signet_audit_logger_free(logger);

  /* Verify file was created */
  FILE *f = fopen(test_path, "r");
  CHECK(f != NULL);
  
  char line[1024];
  int line_count = 0;
  while (fgets(line, sizeof(line), f)) {
    line_count++;
  }
  fclose(f);

  CHECK(line_count >= 2);

  /* Cleanup */
  unlink(test_path);
  CHECK(rmdir(directory) == 0);

  printf("test_audit_logger_file: PASS\n");
}

static void test_audit_logger_null_safety(void) {
  /* NULL logger should not crash */
  int rc = signet_audit_log_json(NULL, SIGNET_AUDIT_EVENT_STARTUP, "{\"test\":\"message\"}");
  CHECK(rc == -1);

  signet_audit_logger_free(NULL);

  printf("test_audit_logger_null_safety: PASS\n");
}

int main(void) {
  test_audit_logger_basic();
  test_audit_logger_file();
  test_audit_logger_null_safety();
  printf("All audit logger tests passed!\n");
  return 0;
}
