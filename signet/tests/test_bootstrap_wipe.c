/* SPDX-License-Identifier: MIT */
#include "../src/secret_json.h"
#include "../src/bootstrap_server.c"
#define main exercise_bootstrap
#include "test_bootstrap_security.c"
#undef main

static const void *expected;
static size_t expected_size;
static gint expected_seen, long_buffers;
static void observe_wipe(const void *data, size_t size) {
  const unsigned char *bytes = data;
  for (size_t i = 0; i < size; i++) CHECK(bytes[i] == 0);
  if (data == expected && size == expected_size) g_atomic_int_inc(&expected_seen);
  if (size > 128) g_atomic_int_inc(&long_buffers);
}
int main(void) {
  signet_secret_json_set_wipe_hook(observe_wipe);
  JsonObject *object = json_object_new();
  json_object_set_string_member(object, "uri", "bunker://fixture?secret=private-pairing");
  expected = json_object_get_string_member(object, "uri"); expected_size = strlen(expected);
  char *serialized = json_response(object);
  CHECK(g_atomic_int_get(&expected_seen) > 0);
  CHECK(strstr(serialized, "private-pairing"));
  signet_secret_gfree(serialized);
  expected = NULL;
  /* Full allocation length, not strlen: a malformed body can contain NUL. */
  PostCtx *pc = g_new0(PostCtx, 1); pc->cap = 100; pc->len = 20;
  pc->buf = g_malloc0(pc->cap + 1); memcpy(pc->buf + 10, "raw-secret", 10);
  expected = pc->buf; expected_size = pc->cap + 1; g_atomic_int_set(&expected_seen, 0);
  void *ctx = pc;
  signet_post_ctx_free(NULL, NULL, &ctx, MHD_REQUEST_TERMINATED_COMPLETED_OK);
  CHECK(!ctx && g_atomic_int_get(&expected_seen) == 1); expected = NULL;
  /* Real HTTP parsing, successful and rejected bootstrap, response ownership,
   * and connection completion all execute with the live-storage observer. */
  CHECK(!exercise_bootstrap());
  CHECK(g_atomic_int_get(&long_buffers) >= 3);
  signet_secret_json_set_wipe_hook(NULL);
  puts("bootstrap JSON trees, full request buffers and HTTP response deallocation wipe: PASS");
  return 0;
}
