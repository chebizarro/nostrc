/* test_profile_metadata.c — kind-0 content + event parser coverage. */
#include "nostr_profile.h"

#include <stdio.h>
#include <string.h>

#define FAIL(msg) do { fprintf(stderr, "FAIL %s\n", msg); return 1; } while (0)
#define OK(msg)   do { printf("ok  %s\n", msg); } while (0)

static int test_content_basic(void) {
  nh_profile_metadata m;
  memset(&m, 0, sizeof m);
  const char *j = "{\"name\":\"alice\",\"display_name\":\"Alice Smith\","
                  "\"picture\":\"https://ex.com/a.png\",\"nip05\":\"alice@ex.com\"}";
  if (nh_profile_parse_content_json(j, &m) != NH_PROFILE_OK) FAIL("basic parse");
  if (strcmp(m.name, "alice") != 0) FAIL("name");
  if (strcmp(m.display_name, "Alice Smith") != 0) FAIL("display_name");
  if (strcmp(m.picture_url, "https://ex.com/a.png") != 0) FAIL("picture");
  if (strcmp(m.nip05, "alice@ex.com") != 0) FAIL("nip05");
  OK("content_basic");
  return 0;
}

static int test_content_damus_camelcase(void) {
  nh_profile_metadata m;
  memset(&m, 0, sizeof m);
  const char *j = "{\"name\":\"a\",\"displayName\":\"A. Bee\"}";
  if (nh_profile_parse_content_json(j, &m) != NH_PROFILE_OK) FAIL("parse");
  if (strcmp(m.display_name, "A. Bee") != 0) FAIL("displayName fallback");
  OK("content_damus_camelcase");
  return 0;
}

static int test_content_rejects_http_picture(void) {
  nh_profile_metadata m;
  memset(&m, 0, sizeof m);
  const char *j = "{\"picture\":\"http://ex.com/a.png\"}";
  if (nh_profile_parse_content_json(j, &m) != NH_PROFILE_OK) FAIL("parse");
  if (m.picture_url[0] != '\0') FAIL("http URL leaked through");
  OK("content_rejects_http_picture");
  return 0;
}

static int test_content_rejects_control_chars(void) {
  nh_profile_metadata m;
  memset(&m, 0, sizeof m);
  /* Injecting a CRLF into the name must not survive the sanitiser. */
  const char *j = "{\"name\":\"hi\\r\\n<inject>\"}";
  if (nh_profile_parse_content_json(j, &m) != NH_PROFILE_OK) FAIL("parse");
  if (m.name[0] != '\0') FAIL("CR/LF name survived");
  OK("content_rejects_control_chars");
  return 0;
}

static int test_content_bad_json(void) {
  nh_profile_metadata m;
  memset(&m, 0, sizeof m);
  if (nh_profile_parse_content_json("not json", &m) != NH_PROFILE_ERR_PARSE)
    FAIL("bad JSON accepted");
  if (nh_profile_parse_content_json("[]", &m) != NH_PROFILE_ERR_PARSE)
    FAIL("array accepted");
  OK("content_bad_json");
  return 0;
}

static int test_event_wrapper(void) {
  nh_profile_metadata m;
  memset(&m, 0, sizeof m);
  /* content field carries an escaped JSON string per NIP-01. */
  const char *event =
      "{\"kind\":0,"
       "\"pubkey\":\"cdee943cbb19c51ab847a66d5d774373aa9f63d287246bb59b0827fa5e637400\","
       "\"created_at\":1700000000,"
       "\"tags\":[],"
       "\"content\":\"{\\\"display_name\\\":\\\"Biz\\\","
                    "\\\"picture\\\":\\\"https://ex.com/p.png\\\"}\","
       "\"id\":\"deadbeef\",\"sig\":\"cafebabe\"}";
  if (nh_profile_parse_event_json(event, &m) != NH_PROFILE_OK) FAIL("event parse");
  if (strcmp(m.display_name, "Biz") != 0) FAIL("event display_name");
  if (strcmp(m.picture_url, "https://ex.com/p.png") != 0) FAIL("event picture");
  if (m.created_at != 1700000000) FAIL("created_at");
  if (strcmp(m.pubkey_hex,
             "cdee943cbb19c51ab847a66d5d774373aa9f63d287246bb59b0827fa5e637400") != 0)
    FAIL("pubkey");
  OK("event_wrapper");
  return 0;
}

static int test_event_wrong_kind(void) {
  nh_profile_metadata m;
  memset(&m, 0, sizeof m);
  const char *event =
      "{\"kind\":1,\"pubkey\":\"cdee943cbb19c51ab847a66d5d774373aa9f63d287246bb59b0827fa5e637400\","
       "\"created_at\":1,\"tags\":[],\"content\":\"{}\",\"id\":\"x\",\"sig\":\"y\"}";
  if (nh_profile_parse_event_json(event, &m) != NH_PROFILE_ERR_PARSE)
    FAIL("wrong-kind event accepted");
  OK("event_wrong_kind");
  return 0;
}

int main(void) {
  if (test_content_basic() != 0) return 1;
  if (test_content_damus_camelcase() != 0) return 1;
  if (test_content_rejects_http_picture() != 0) return 1;
  if (test_content_rejects_control_chars() != 0) return 1;
  if (test_content_bad_json() != 0) return 1;
  if (test_event_wrapper() != 0) return 1;
  if (test_event_wrong_kind() != 0) return 1;
  puts("test_profile_metadata: ALL PASS");
  return 0;
}
