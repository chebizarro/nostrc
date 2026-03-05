/* SPDX-License-Identifier: MIT
 *
 * test_audit_logger.c - Audit logger tests
 */

#include "signet/audit_logger.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_audit_logger_basic(void) {
  /* Create logger to stdout */
  SignetAuditLoggerConfig cfg = {
    .path = NULL,
    .to_stdout = true,
    .flush_each_write = true,
  };

  SignetAuditLogger *logger = signet_audit_logger_new(&cfg);
  assert(logger != NULL);

  /* Write a basic audit entry */
  int rc = signet_audit_log_json(logger, SIGNET_AUDIT_EVENT_STARTUP, "{\"test\":\"message\"}");
  assert(rc == 0);

  signet_audit_logger_free(logger);
  printf("test_audit_logger_basic: PASS\n");
}

static void test_audit_logger_file(void) {
  const char *test_path = "/tmp/signet_test_audit.jsonl";
  
  /* Remove old test file if exists */
  unlink(test_path);

  SignetAuditLoggerConfig cfg = {
    .path = test_path,
    .to_stdout = false,
    .flush_each_write = true,
  };

  SignetAuditLogger *logger = signet_audit_logger_new(&cfg);
  assert(logger != NULL);

  /* Write multiple entries */
  signet_audit_log_json(logger, SIGNET_AUDIT_EVENT_STARTUP, "{\"event\":\"1\"}");
  signet_audit_log_json(logger, SIGNET_AUDIT_EVENT_SHUTDOWN, "{\"event\":\"2\"}");

  signet_audit_logger_free(logger);

  /* Verify file was created */
  FILE *f = fopen(test_path, "r");
  assert(f != NULL);
  
  char line[1024];
  int line_count = 0;
  while (fgets(line, sizeof(line), f)) {
    line_count++;
  }
  fclose(f);

  assert(line_count >= 2);

  /* Cleanup */
  unlink(test_path);

  printf("test_audit_logger_file: PASS\n");
}

static void test_audit_logger_null_safety(void) {
  /* NULL logger should not crash */
  int rc = signet_audit_log_json(NULL, SIGNET_AUDIT_EVENT_STARTUP, "{\"test\":\"message\"}");
  assert(rc == -1);

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
