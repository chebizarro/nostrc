/* SPDX-License-Identifier: MIT */

#include "signet/memory_hardening.h"

#include <assert.h>
#include <stdio.h>

static void expect_mode(const char *raw, SignetMlockMode want) {
  int valid = 0;
  SignetMlockMode got = signet_mlock_mode_from_env(raw, &valid);
  assert(valid == 1);
  assert(got == want);
}

static void test_mlock_mode_parser(void) {
  expect_mode(NULL, SIGNET_MLOCK_MODE_FUTURE);
  expect_mode("", SIGNET_MLOCK_MODE_FUTURE);
  expect_mode("future", SIGNET_MLOCK_MODE_FUTURE);
  expect_mode("full", SIGNET_MLOCK_MODE_FUTURE);
  expect_mode("current", SIGNET_MLOCK_MODE_CURRENT);
  expect_mode("off", SIGNET_MLOCK_MODE_OFF);
  expect_mode("none", SIGNET_MLOCK_MODE_OFF);
  expect_mode("false", SIGNET_MLOCK_MODE_OFF);
  expect_mode("0", SIGNET_MLOCK_MODE_OFF);

  int valid = 1;
  SignetMlockMode got = signet_mlock_mode_from_env("bogus", &valid);
  assert(valid == 0);
  assert(got == SIGNET_MLOCK_MODE_FUTURE);
  printf("test_mlock_mode_parser: PASS\n");
}

static void test_mlock_mode_names(void) {
  assert(signet_mlock_mode_to_string(SIGNET_MLOCK_MODE_FUTURE));
  assert(signet_mlock_mode_to_string(SIGNET_MLOCK_MODE_CURRENT));
  assert(signet_mlock_mode_to_string(SIGNET_MLOCK_MODE_OFF));
  printf("test_mlock_mode_names: PASS\n");
}

int main(void) {
  test_mlock_mode_parser();
  test_mlock_mode_names();
  return 0;
}
