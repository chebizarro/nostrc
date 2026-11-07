/* Compile the actual CLI builder to catch ownership/zeroization regressions. */
#define main signetctl_entrypoint
#include "../src/signetctl_main.c"
#undef main
#include "test_check.h"

int main(void) {
  const char *policy = "{\"default\":\"deny\",\"methods\":[\"sign_event\"],\"nested\":{\"client\":\"client-key\"}}";
  char *intent = signetctl_build_intent(SIGNET_MGMT_OP_SET_POLICY, "agent", "id",
      policy, NULL, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, false, 0, NULL);
  CHECK(intent);
  JsonParser *parser = json_parser_new();
  CHECK(json_parser_load_from_data(parser, intent, -1, NULL));
  JsonObject *root = json_node_get_object(json_parser_get_root(parser));
  JsonObject *params = json_object_get_object_member(root, "params");
  JsonObject *sent = json_object_get_object_member(params, "policy");
  CHECK(strcmp(json_object_get_string_member(sent, "default"), "deny") == 0);
  CHECK(strcmp(json_array_get_string_element(json_object_get_array_member(sent, "methods"), 0), "sign_event") == 0);
  CHECK(strcmp(json_object_get_string_member(json_object_get_object_member(sent, "nested"), "client"), "client-key") == 0);
  CHECK(strstr(policy, "client-key")); /* borrowed input remains caller-owned */
  signetctl_wipe_free_string(intent);
  signetctl_secret_parser_free(parser);

  const char *uri = "bunker://public?secret=bearer-must-not-leak";
  FILE *capture = tmpfile(); CHECK(capture);
  int saved = dup(STDOUT_FILENO); CHECK(saved >= 0);
  fflush(stdout); CHECK(dup2(fileno(capture), STDOUT_FILENO) >= 0);
  char *reply = g_strdup_printf("{\"result\":{\"agent_id\":\"agent\",\"bunker_uri\":\"%s\"}}", uri);
  signetctl_print_adopt_result(reply);
  fflush(stdout); CHECK(dup2(saved, STDOUT_FILENO) >= 0); close(saved);
  CHECK(fseek(capture, 0, SEEK_SET) == 0);
  char output[512] = {0}; CHECK(fread(output, 1, sizeof(output) - 1, capture) > 0);
  CHECK(!strstr(output, "bearer-must-not-leak"));
  CHECK(strstr(output, "redacted"));
  fclose(capture); signetctl_wipe_free_string(reply);
  char *dir = g_dir_make_tmp("signet-uri-XXXXXX", NULL); CHECK(dir);
  char *path = g_build_filename(dir, "handoff", NULL);
  mode_t old = umask(0);
  CHECK(signetctl_write_secret_file(path, uri) == 0);
  umask(old);
  struct stat st; CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
  unlink(path); rmdir(dir); g_free(path); g_free(dir);
  return 0;
}
