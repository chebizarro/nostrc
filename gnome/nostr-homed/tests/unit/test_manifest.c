#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "nostr_manifest.h"

int main(void){
  const char *js = "{\n"
                   "  \"version\":2,\n"
                   "  \"entries\":[{\"path\":\"/README.txt\",\"cid\":\"abc\",\"size\":5,\"meta\":{\"mode\":420,\"mtime\":123,\"uid\":1000,\"gid\":1000}}],\n"
                   "  \"links\":[{\"path\":\"/docs\",\"manifest_event_ref\":\"%s:30081:personal\"}]\n"
                   "}";
  nh_manifest m; int rc = nh_manifest_parse_json(js, &m);
  if (rc != 0) { fprintf(stderr, "nh_manifest_parse_json failed: %d\n", rc); return 1; }
  assert(rc == 0);
  assert(m.version == 2);
  assert(m.entries_len == 1);
  assert(strcmp(m.entries[0].path, "/README.txt")==0);
  assert(strcmp(m.entries[0].cid, "abc")==0);
  assert(m.entries[0].size == 5);
  assert(m.entries[0].mode == 420);
  assert(m.entries[0].mtime == 123);
  assert(m.links_len == 1);
  assert(strcmp(m.links[0].path, "/docs")==0);
  nh_manifest_free(&m);
  puts("ok");
  return 0;
}
