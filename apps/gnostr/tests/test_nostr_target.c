/*
 * test_nostr_target.c — nostr: link routing + org.nostr.Handler1 export
 * (nostrc-prqu.3).
 *
 * Headless: URI parsing and refusals, the kind -> view table against the
 * desktop file's X-Nostr-Kinds=, event validation, naddr candidate choice
 * (stale vs rotated), and the Handler1 D-Bus contract on a private bus.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "util/gnostr-nostr-target.h"
#include "ipc/gnostr-handler1.h"

#include <nostr-gobject-1.0/nostr_nip19.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-tag.h>
#include <stdlib.h>
#include <string.h>

#define SK "7f7ff03d123792d6ac594bfa67bf6d0c0ab55b6b1fdb6249303fe861f1ccba9a"

static char *s_pk;

static char *
signed_event(int kind, gint64 created_at, const char *d, const char *content)
{
  NostrEvent *ev = nostr_event_new();
  nostr_event_set_kind(ev, kind);
  nostr_event_set_created_at(ev, created_at);
  nostr_event_set_content(ev, content);
  NostrTags *tags = d ? nostr_tags_new(1, nostr_tag_new("d", d, NULL)) : nostr_tags_new(0);
  nostr_event_set_tags(ev, tags);
  g_assert_cmpint(nostr_event_sign(ev, SK), ==, 0);
  char *json = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);
  return json;
}

static char *
uri_for(GNostrNip19 *n19)
{
  g_assert_nonnull(n19);
  char *uri = g_strconcat("nostr:", gnostr_nip19_get_bech32(n19), NULL);
  g_object_unref(n19);
  return uri;
}

static void
test_parse_profiles(void)
{
  g_autofree char *npub = uri_for(gnostr_nip19_encode_npub(s_pk, NULL));
  g_autoptr(GnostrNostrTarget) t = gnostr_nostr_target_parse(npub, NULL);
  g_assert_nonnull(t);
  g_assert_cmpint(t->type, ==, GNOSTR_NOSTR_TARGET_PROFILE);
  g_assert_cmpstr(t->pubkey_hex, ==, s_pk);
  g_assert_cmpint(t->kind, ==, 0);
  g_assert_null(t->relays[0]);

  const char *relays[] = { "wss://relay.example", NULL };
  g_autofree char *nprofile = uri_for(gnostr_nip19_encode_nprofile(s_pk, relays, NULL));
  /* web+nostr: and an upper-case scheme are the same link. */
  g_autofree char *web = g_strconcat("web+NOSTR:", nprofile + 6, "?x=1#frag", NULL);
  g_autoptr(GnostrNostrTarget) p = gnostr_nostr_target_parse(web, NULL);
  g_assert_nonnull(p);
  g_assert_cmpint(p->type, ==, GNOSTR_NOSTR_TARGET_PROFILE);
  g_assert_cmpstr(p->pubkey_hex, ==, s_pk);
  g_assert_cmpstr(p->relays[0], ==, "wss://relay.example");
  g_assert_null(p->relays[1]);
}

static void
test_parse_events(void)
{
  const char *id = "b9f5441e45ca39179320e0031cfb18e34078673dcc3d3e3a3b3a981760aa5696";
  const char *relays[] = { "wss://a.example", NULL };

  g_autofree char *note = uri_for(gnostr_nip19_encode_note(id, NULL));
  g_autoptr(GnostrNostrTarget) n = gnostr_nostr_target_parse(note, NULL);
  g_assert_nonnull(n);
  g_assert_cmpint(n->type, ==, GNOSTR_NOSTR_TARGET_EVENT);
  g_assert_cmpstr(n->event_id_hex, ==, id);
  g_assert_cmpint(n->kind, ==, -1);

  g_autofree char *nev = uri_for(gnostr_nip19_encode_nevent(id, relays, s_pk, 30023, NULL));
  g_autoptr(GnostrNostrTarget) e = gnostr_nostr_target_parse(nev, NULL);
  g_assert_nonnull(e);
  g_assert_cmpint(e->type, ==, GNOSTR_NOSTR_TARGET_EVENT);
  g_assert_cmpint(e->kind, ==, 30023);
  g_assert_cmpstr(e->pubkey_hex, ==, s_pk);
  g_assert_cmpstr(e->relays[0], ==, "wss://a.example");

  g_autofree char *naddr = uri_for(gnostr_nip19_encode_naddr("my-article", s_pk, 30023, relays, NULL));
  g_autoptr(GnostrNostrTarget) a = gnostr_nostr_target_parse(naddr, NULL);
  g_assert_nonnull(a);
  g_assert_cmpint(a->type, ==, GNOSTR_NOSTR_TARGET_ADDRESS);
  g_assert_cmpint(a->kind, ==, 30023);
  g_assert_cmpstr(a->pubkey_hex, ==, s_pk);
  g_assert_cmpstr(a->d_tag, ==, "my-article");
}

static void
test_parse_refusals(void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GnostrNostrTarget) t = NULL;

  g_autoptr(GNostrNip19) nsec = gnostr_nip19_encode_nsec(SK, NULL);
  g_assert_nonnull(nsec);
  g_autofree char *nsec_uri = g_strconcat("nostr:", gnostr_nip19_get_bech32(nsec), NULL);
  t = gnostr_nostr_target_parse(nsec_uri, &error);
  g_assert_null(t);
  g_assert_error(error, GNOSTR_NOSTR_TARGET_ERROR, GNOSTR_NOSTR_TARGET_ERROR_REFUSED);
  g_clear_error(&error);

  t = gnostr_nostr_target_parse("nostr:ncryptsec1qgg9947rlpvqu76pj5ecreduf9jxhselq2nae2kghhvd5g7dgjtcxfqtd67p9m0w57lspw8gsq6yphnm8623nsl8xn9j4jdzz84zm3frztj3z7s35vpzmqf6ksu8r89qk5z2zxfmu5gv8th8wclt0h4p", &error);
  g_assert_null(t);
  g_assert_error(error, GNOSTR_NOSTR_TARGET_ERROR, GNOSTR_NOSTR_TARGET_ERROR_REFUSED);
  g_clear_error(&error);

  const char *relays[] = { "wss://r.example", NULL };
  g_autoptr(GNostrNip19) nrelay = gnostr_nip19_encode_nrelay(relays, NULL);
  if (nrelay) {
    g_autofree char *nrelay_uri = g_strconcat("nostr:", gnostr_nip19_get_bech32(nrelay), NULL);
    t = gnostr_nostr_target_parse(nrelay_uri, &error);
    g_assert_null(t);
    g_assert_error(error, GNOSTR_NOSTR_TARGET_ERROR, GNOSTR_NOSTR_TARGET_ERROR_UNSUPPORTED);
    g_clear_error(&error);
  }

  const char *bad[] = { NULL, "", "nostr:", "https://example.com", "nostr://open?event=abc",
                        "nostr:npub1notvalid", "file:///tmp/x.nostr" };
  for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) {
    t = gnostr_nostr_target_parse(bad[i], &error);
    g_assert_null(t);
    g_assert_error(error, GNOSTR_NOSTR_TARGET_ERROR, GNOSTR_NOSTR_TARGET_ERROR_INVALID);
    g_clear_error(&error);
  }
}

static void
test_views_match_desktop_file(void)
{
  g_assert_cmpint(gnostr_nostr_view_for_kind(0), ==, GNOSTR_NOSTR_VIEW_PROFILE);
  g_assert_cmpint(gnostr_nostr_view_for_kind(1), ==, GNOSTR_NOSTR_VIEW_THREAD);
  g_assert_cmpint(gnostr_nostr_view_for_kind(1059), ==, GNOSTR_NOSTR_VIEW_MESSAGES);
  g_assert_cmpint(gnostr_nostr_view_for_kind(14), ==, GNOSTR_NOSTR_VIEW_MESSAGES);
  g_assert_cmpint(gnostr_nostr_view_for_kind(30023), ==, GNOSTR_NOSTR_VIEW_ARTICLE);
  g_assert_cmpint(gnostr_nostr_view_for_kind(31337), ==, GNOSTR_NOSTR_VIEW_THREAD); /* `*` */
  g_assert_cmpint(gnostr_nostr_view_for_kind(-1), ==, GNOSTR_NOSTR_VIEW_THREAD);

  g_autoptr(GKeyFile) kf = g_key_file_new();
  g_autoptr(GError) error = NULL;
  g_assert_true(g_key_file_load_from_file(kf, GNOSTR_DESKTOP_FILE, G_KEY_FILE_NONE, &error));
  g_assert_no_error(error);
  g_auto(GStrv) tokens = g_key_file_get_string_list(kf, "Desktop Entry", "X-Nostr-Kinds",
                                                    NULL, &error);
  g_assert_no_error(error);

  gsize n = 0;
  const int *kinds = gnostr_nostr_declared_kinds(&n);
  gsize numeric = 0;
  gboolean star = FALSE;
  for (gsize i = 0; tokens[i]; i++) {
    if (g_strcmp0(tokens[i], "*") == 0) {
      star = TRUE;
      continue;
    }
    g_assert_cmpuint(numeric, <, n);
    g_assert_cmpint(atoi(tokens[i]), ==, kinds[numeric]);
    numeric++;
  }
  g_assert_cmpuint(numeric, ==, n);
  g_assert_true(star);  /* GNostr stays the generic fallback viewer */

  /* Exec takes the URI; nostr: itself belongs to nostr-dispatcher. */
  g_autofree char *exec = g_key_file_get_string(kf, "Desktop Entry", "Exec", NULL);
  g_assert_nonnull(strstr(exec, "%U"));
  g_autofree char *mime = g_key_file_get_string(kf, "Desktop Entry", "MimeType", NULL);
  g_assert_true(mime == NULL || strstr(mime, "x-scheme-handler/nostr") == NULL);
}

static void
test_event_validation(void)
{
  g_autofree char *json = signed_event(1, 1700000000, NULL, "hello");
  g_autoptr(GError) error = NULL;
  g_autoptr(GnostrNostrEventInfo) info = gnostr_nostr_event_parse(json, &error);
  g_assert_no_error(error);
  g_assert_nonnull(info);
  g_assert_cmpint(info->kind, ==, 1);
  g_assert_cmpstr(info->pubkey_hex, ==, s_pk);

  /* Any tampering breaks the id/signature. */
  g_autoptr(GString) bad = g_string_new(json);
  g_string_replace(bad, "hello", "HELLO", 1);
  g_assert_null(gnostr_nostr_event_parse(bad->str, &error));
  g_assert_error(error, GNOSTR_NOSTR_TARGET_ERROR, GNOSTR_NOSTR_TARGET_ERROR_INVALID);
  g_clear_error(&error);
  g_assert_null(gnostr_nostr_event_parse("{\"kind\":1}", &error));
  g_clear_error(&error);
  g_assert_null(gnostr_nostr_event_parse("not json", NULL));
}

static void
test_pick_naddr_candidates(void)
{
  g_autofree char *stale = signed_event(30023, 1700000000, "art", "v1");
  g_autofree char *rotated = signed_event(30023, 1700000500, "art", "v2");
  g_autofree char *other_d = signed_event(30023, 1700009999, "other", "no");
  g_autofree char *other_kind = signed_event(30024, 1700009999, "art", "draft");

  GnostrNostrTarget t = { .type = GNOSTR_NOSTR_TARGET_ADDRESS, .pubkey_hex = s_pk,
                          .kind = 30023, .d_tag = (char *)"art" };
  const char *all[] = { stale, other_d, "garbage", rotated, other_kind };
  g_autofree char *picked = gnostr_nostr_pick_event_for_target(&t, all, G_N_ELEMENTS(all));
  g_assert_cmpstr(picked, ==, rotated);

  const char *none[] = { other_d, other_kind };
  g_assert_null(gnostr_nostr_pick_event_for_target(&t, none, G_N_ELEMENTS(none)));

  /* An event target only accepts that id (and the link's kind). */
  g_autoptr(GnostrNostrEventInfo) info = gnostr_nostr_event_parse(stale, NULL);
  GnostrNostrTarget e = { .type = GNOSTR_NOSTR_TARGET_EVENT, .event_id_hex = info->id_hex,
                          .kind = -1 };
  g_assert_true(gnostr_nostr_event_matches_target(info, &e));
  e.kind = 1;
  g_assert_false(gnostr_nostr_event_matches_target(info, &e));
  const char *wrong[] = { rotated };
  e.kind = -1;
  g_assert_null(gnostr_nostr_pick_event_for_target(&e, wrong, 1));
}

/* ---- Handler1 on a private bus --------------------------------------- */

typedef struct {
  guint calls;
  guint kind;
  char *json;
  char **relays;
} Seen;

static gboolean
fake_open(guint kind, const char *event_json, const char *const *relays,
          gpointer user_data, GError **error)
{
  Seen *seen = user_data;
  seen->calls++;
  seen->kind = kind;
  g_free(seen->json);
  seen->json = g_strdup(event_json);
  g_strfreev(seen->relays);
  seen->relays = g_strdupv((char **)relays);
  if (kind == 7) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "nope");
    return FALSE;
  }
  return TRUE;
}

typedef struct {
  gboolean done;
  GVariant *reply;
  GError *error;
} Call;

static void
on_call_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  Call *c = user_data;
  c->reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &c->error);
  c->done = TRUE;
}

/* Call OpenEvent asynchronously and spin the (shared) main context: the
 * exported object dispatches there too, so a sync call would deadlock. */
static void
call_open_event(GDBusConnection *caller, GVariant *params, Call *c)
{
  memset(c, 0, sizeof *c);
  g_dbus_connection_call(caller, GNOSTR_HANDLER1_BUS_NAME, "/org/gnostr/gnostr",
                         "org.nostr.Handler1", "OpenEvent", params, NULL,
                         G_DBUS_CALL_FLAGS_NO_AUTO_START, 5000, NULL, on_call_done, c);
  while (!c->done)
    g_main_context_iteration(NULL, TRUE);
}

static void
test_handler1_export(void)
{
  g_autoptr(GTestDBus) bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(bus);
  g_autoptr(GError) error = NULL;
  g_autoptr(GDBusConnection) app_conn =
      g_dbus_connection_new_for_address_sync(g_test_dbus_get_bus_address(bus),
          G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, &error);
  g_assert_no_error(error);
  g_autoptr(GDBusConnection) caller =
      g_dbus_connection_new_for_address_sync(g_test_dbus_get_bus_address(bus),
          G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION, NULL, NULL, &error);
  g_assert_no_error(error);

  Seen seen = { 0 };
  GnostrHandler1 *h = gnostr_handler1_export(app_conn, fake_open, &seen, &error);
  g_assert_no_error(error);
  g_assert_nonnull(h);

  /* The dispatcher's liveness probe: NameHasOwner(desktop id). */
  gboolean owned = FALSE;
  for (int i = 0; i < 200 && !owned; i++) {
    g_main_context_iteration(NULL, FALSE);
    g_autoptr(GVariant) r = g_dbus_connection_call_sync(caller, "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
        g_variant_new("(s)", GNOSTR_HANDLER1_BUS_NAME), G_VARIANT_TYPE("(b)"),
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    if (r) g_variant_get(r, "(b)", &owned);
    if (!owned) g_usleep(10000);
  }
  g_assert_true(owned);

  /* OpenEvent(u kind, s event_json, as relays) at the path the dispatcher
   * derives from the desktop id (org.gnostr.gnostr -> /org/gnostr/gnostr). */
  const char *relays[] = { "wss://hint.example", NULL };
  Call c;
  call_open_event(caller, g_variant_new("(us^as)", 1u, "{\"kind\":1}", relays), &c);
  g_assert_no_error(c.error);
  g_clear_pointer(&c.reply, g_variant_unref);
  g_assert_cmpuint(seen.calls, ==, 1);
  g_assert_cmpuint(seen.kind, ==, 1);
  g_assert_cmpstr(seen.json, ==, "{\"kind\":1}");
  g_assert_cmpstr(seen.relays[0], ==, "wss://hint.example");

  /* The app rejects: a D-Bus error, so the dispatcher launches the URI. */
  call_open_event(caller, g_variant_new("(us^as)", 7u, "{}", relays), &c);
  g_assert_error(c.error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS);
  g_clear_error(&c.error);
  g_assert_cmpuint(seen.calls, ==, 2);

  /* Out-of-range kinds never reach the app. */
  call_open_event(caller, g_variant_new("(us^as)", 70000u, "{}", relays), &c);
  g_assert_error(c.error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS);
  g_clear_error(&c.error);
  g_assert_cmpuint(seen.calls, ==, 2);

  /* Wrong signature: GDBus refuses before our handler runs. */
  call_open_event(caller, g_variant_new("(ss)", "1", "{}"), &c);
  g_assert_nonnull(c.error);
  g_clear_error(&c.error);
  g_assert_cmpuint(seen.calls, ==, 2);

  /* Rate limit: 10 accepted calls per window, then LimitsExceeded. */
  for (int i = 0; i < 8; i++) {
    call_open_event(caller, g_variant_new("(us^as)", 1u, "{}", relays), &c);
    g_assert_no_error(c.error);
    g_clear_pointer(&c.reply, g_variant_unref);
  }
  call_open_event(caller, g_variant_new("(us^as)", 1u, "{}", relays), &c);
  g_assert_error(c.error, G_DBUS_ERROR, G_DBUS_ERROR_LIMITS_EXCEEDED);
  g_clear_error(&c.error);
  g_assert_cmpuint(seen.calls, ==, 10);

  gnostr_handler1_unexport(h);
  g_free(seen.json);
  g_strfreev(seen.relays);
  g_test_dbus_down(bus);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  s_pk = nostr_key_get_public(SK);
  g_assert_nonnull(s_pk);

  g_test_add_func("/nostr-target/parse-profiles", test_parse_profiles);
  g_test_add_func("/nostr-target/parse-events", test_parse_events);
  g_test_add_func("/nostr-target/parse-refusals", test_parse_refusals);
  g_test_add_func("/nostr-target/views-match-desktop-file", test_views_match_desktop_file);
  g_test_add_func("/nostr-target/event-validation", test_event_validation);
  g_test_add_func("/nostr-target/pick-naddr-candidates", test_pick_naddr_candidates);
  g_test_add_func("/nostr-target/handler1-export", test_handler1_export);
  int rc = g_test_run();
  free(s_pk);
  return rc;
}
