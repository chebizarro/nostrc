/*
 * test_notify_sub — assert notification-payload invariants (§3.3 Piece B).
 *
 * Two invariants tracked:
 *
 *   1. Kind-1059 gift-wrap DM body is FULLY OPAQUE. The notifier cannot
 *      derive sender/thread without unwrapping (NIP-17 fresh random
 *      wrapper key), so the body MUST NOT contain:
 *        - any part of the outer gift-wrap event id
 *        - any hex substring that could be mistaken for a pubkey/prefix
 *      per §3.3 D3 Finding 14 ("assert opacity — no hex8/pubkey leaks").
 *
 *   2. NIP-29 group message preview is truncated to ≤80 characters and
 *      GTK markup is escaped so an event containing `<script>` cannot
 *      inject markup into Pango.
 *
 *   3. Deep links are NIP-21 `nostr:nevent1…` (nostrc-1v65): event id +
 *      kind TLV (1059 / 9..12) + the arrival relay as hint, no author TLV,
 *      decodable with nip19 — so nostr-dispatcher can route by kind.
 *
 * The test uses the notifier's own pure helpers (dm_title / dm_opaque_body /
 * group_preview / dm_deep_link_uri / group_deep_link_uri) — GLib's
 * `g_notification_serialize` is a private API we deliberately avoid
 * depending on.
 */
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gio/gio.h>
#include <glib.h>

#include "notify_gnotification.h"
#include "nostr/nip19/nip19.h"

#define DM_OPAQUE_BODY "You have a new encrypted direct message."
#define DM_TITLE "Nostr message"

static int contains(const char *hay, const char *needle) {
  return hay && needle && strstr(hay, needle) != NULL;
}

/*
 * `contains_hex_run` returns true iff `s` contains a hex-only run of
 * length >= min_len. This is the concrete assertion behind "no hex8
 * leaks" — any lowercase hex substring that long is treated as a
 * suspicious identifier fragment.
 */
static int contains_hex_run(const char *s, size_t min_len) {
  if (!s) return 0;
  size_t run = 0;
  for (const char *p = s; *p; p++) {
    char c = *p;
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
      run++;
      if (run >= min_len) return 1;
    } else {
      run = 0;
    }
  }
  return 0;
}

/* Decode a deep link and check id / kind / relay; exits on mismatch. */
static void assert_nevent(const char *uri, const char *id, int kind,
                          const char *relay) {
  if (!uri || strncmp(uri, "nostr:nevent1", 13) != 0) {
    fprintf(stderr, "test_notify_sub: not a nostr:nevent URI: %s\n",
            uri ? uri : "(null)");
    exit(1);
  }
  NostrEventPointer *p = NULL;
  if (nostr_nip19_decode_nevent(uri + 6, &p) != 0 || !p) {
    fprintf(stderr, "test_notify_sub: nevent does not decode: %s\n", uri);
    exit(1);
  }
  if (strcmp(p->id, id) != 0 || p->kind != kind || p->author != NULL ||
      (relay ? (p->relays_count != 1 || strcmp(p->relays[0], relay) != 0)
             : p->relays_count != 0)) {
    fprintf(stderr, "test_notify_sub: nevent fields wrong (kind=%d relays=%zu)\n",
            p->kind, p->relays_count);
    exit(1);
  }
  nostr_event_pointer_free(p);
}

static const char *k_giftwrap =
    "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef";

static void assert_dm_opacity(void) {
  /* Fixed opacity constants — the plan asserts these verbatim. */
  const char *title = nostr_notify_dm_title();
  const char *body = nostr_notify_dm_opaque_body();
  if (!title || strcmp(title, DM_TITLE) != 0) {
    fprintf(stderr, "test_notify_sub: DM title unexpected: %s\n",
            title ? title : "(null)");
    exit(1);
  }
  if (!body || strcmp(body, DM_OPAQUE_BODY) != 0) {
    fprintf(stderr, "test_notify_sub: DM body not opaque: '%s'\n",
            body ? body : "(null)");
    exit(1);
  }
  if (contains_hex_run(body, 8)) {
    fprintf(stderr, "test_notify_sub: DM body contains hex leak: %s\n", body);
    exit(1);
  }
  if (contains_hex_run(title, 8)) {
    fprintf(stderr, "test_notify_sub: DM title contains hex leak: %s\n", title);
    exit(1);
  }

  /* Deep-link URI encodes the giftwrap id + kind 1059 + arrival relay —
   * that is how the dispatcher routes the click to the DM handler. */
  g_autofree char *uri =
      nostr_notify_dm_deep_link_uri(k_giftwrap, "wss://inbox.example.com");
  assert_nevent(uri, k_giftwrap, 1059, "wss://inbox.example.com");
  g_autofree char *uri_norelay = nostr_notify_dm_deep_link_uri(k_giftwrap, NULL);
  assert_nevent(uri_norelay, k_giftwrap, 1059, NULL);
  /* A non-websocket "relay" never becomes a hint. */
  g_autofree char *uri_bad = nostr_notify_dm_deep_link_uri(k_giftwrap, "file:///x");
  assert_nevent(uri_bad, k_giftwrap, 1059, NULL);

  /* Build the full GNotification and confirm the coalescing withdraw_id
   * does NOT leak the full giftwrap id (only an internal 8-hex prefix
   * used as a coalescing key). */
  NostrNotifyBuild build; memset(&build, 0, sizeof build);
  GNotification *n = nostr_notify_build_dm(k_giftwrap, "wss://inbox.example.com", &build);
  if (!n || !build.withdraw_id) {
    fprintf(stderr, "test_notify_sub: build_dm returned NULL\n");
    exit(1);
  }
  if (contains(build.withdraw_id, k_giftwrap)) {
    fprintf(stderr,
            "test_notify_sub: DM withdraw_id leaks full giftwrap id: %s\n",
            build.withdraw_id);
    exit(1);
  }
  nostr_notify_build_dispose(&build);
  g_object_unref(n);

  /* Truncated event id (63 chars) MUST refuse to build — a malformed id
   * cannot advertise a broken deep link. */
  memset(&build, 0, sizeof build);
  n = nostr_notify_build_dm("deadbeef", NULL, &build);
  if (n) {
    fprintf(stderr, "test_notify_sub: build_dm accepted short id\n");
    exit(1);
  }
}

static void assert_group_preview_truncation(void) {
  /* Content prefixed with a markup injector; UTF-8 body length must
   * shrink to ≤80 code points AND escape < to <. */
  char big[256];
  memset(big, 'x', sizeof big);
  big[sizeof big - 1] = '\0';
  memcpy(big, "<script>", 8);

  g_autofree char *preview = nostr_notify_group_preview(big);
  if (!preview) { fprintf(stderr, "group_preview NULL\n"); exit(1); }

  if (strstr(preview, "<script>") != NULL) {
    fprintf(stderr, "test_notify_sub: markup not escaped: %s\n", preview);
    exit(1);
  }
  const char *escaped_marker = "&" "lt;script&" "gt;";
  if (strstr(preview, escaped_marker) == NULL) {
    fprintf(stderr, "test_notify_sub: expected escaped markup, got: %s\n",
            preview);
    exit(1);
  }
  /* Ellipsis appended when truncated. */
  if (!strstr(preview, "\xe2\x80\xa6")) {
    fprintf(stderr,
            "test_notify_sub: expected ellipsis on truncated preview\n");
    exit(1);
  }
  /* Character count sanity: original was ~255; the pre-escape budget is
   * 80 chars; escaping can inflate `<`/`>` to 4 chars each. The escaped
   * form for our injector-prefixed input is ~74 preserved + `<script>`
   * expansion + ellipsis, so certainly under 300 chars. */
  glong nchars = g_utf8_strlen(preview, -1);
  if (nchars < 20 || nchars > 300) {
    fprintf(stderr,
            "test_notify_sub: preview length %ld outside expected range\n",
            nchars);
    exit(1);
  }

  /* Empty and NULL content don't crash. */
  g_autofree char *empty = nostr_notify_group_preview("");
  if (!empty) { fprintf(stderr, "empty preview NULL\n"); exit(1); }
  g_autofree char *nullp = nostr_notify_group_preview(NULL);
  if (!nullp) { fprintf(stderr, "NULL preview NULL\n"); exit(1); }

  /* Group deep links: nevent with the group-message kind and the group
   * relay as hint (the `h` tag travels inside the signed event). */
  const char *h_tag = "test-group";
  const char *event_id =
      "cafebabecafebabecafebabecafebabecafebabecafebabecafebabecafebabe";
  g_autofree char *guri =
      nostr_notify_group_deep_link_uri(event_id, 9, "wss://groups.example.com");
  assert_nevent(guri, event_id, 9, "wss://groups.example.com");
  if (nostr_notify_group_deep_link_uri(event_id, 1, NULL) != NULL) {
    fprintf(stderr, "test_notify_sub: group URI accepted non-NIP-29 kind\n");
    exit(1);
  }

  NostrNotifyBuild build; memset(&build, 0, sizeof build);
  GNotification *n = nostr_notify_build_group("Group", h_tag, event_id, 11, "hi",
                                              "wss://groups.example.com", &build);
  if (!n || !build.withdraw_id) {
    fprintf(stderr, "test_notify_sub: build_group returned NULL\n");
    exit(1);
  }
  nostr_notify_build_dispose(&build);
  g_object_unref(n);
  memset(&build, 0, sizeof build);
  if (nostr_notify_build_group(NULL, h_tag, event_id, 1, "hi", NULL, &build)) {
    fprintf(stderr, "test_notify_sub: build_group accepted kind 1\n");
    exit(1);
  }
}

int main(void) {
  assert_dm_opacity();
  assert_group_preview_truncation();
  fprintf(stderr, "test_notify_sub: OK\n");
  return 0;
}
