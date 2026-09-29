/* New Message without a display (privacy charter §7.9, PD-2, §8.2 G18):
 * what can be typed or pasted (gh-recipient.h) is classified offline; the
 * NIP-05 URL and document rules; lookup consent, i.e. a recording HTTP
 * transport sees nothing until a lookup is asked for, one bounded GET of the
 * address's own host then, and nothing at all in Tor mode or for .onion; and
 * GhNetHttp, the libsoup client, against a loopback libsoup server: status
 * handling, no redirect followed, the size cap, https only (loopback http
 * for tests), and no User-Agent, Cookie, Referer or Accept-Language sent.
 * Waits iterate the main context against a deadline; they never sleep. */
#include "gh-nip05.h"
#include "gh-recipient.h"

#include <libsoup/soup.h>
#include <nostr-keys.h>
#include <nostr-utils.h>
#include <nostr/nip19/nip19.h>
#include <stdlib.h>
#include <string.h>

#define SK_BOB "0000000000000000000000000000000000000000000000000000000000000002"

static gchar *hex_bob;
static gchar *npub_bob;

static gboolean
deadline_hit(gpointer data)
{
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
}

static void
spin_until_at(gboolean (*pred)(gpointer), gpointer data, int line)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(10, deadline_hit, &expired);
  while (!pred(data) && !expired)
    g_main_context_iteration(NULL, TRUE);
  if (expired)
    g_error("condition waited for at line %d did not hold within 10s", line);
  g_source_remove(timer);
}
#define spin_until(pred, data) spin_until_at((pred), (data), __LINE__)

/* ---- identifiers ---------------------------------------------------------------- */

static GhRecipientInputKind
kind_of(const gchar *text)
{
  g_autoptr(GhRecipientInput) input = gh_recipient_input_parse(text);
  return input->kind;
}

static gchar *
nprofile_of(const gchar *hex, const gchar *relay)
{
  NostrProfilePointer *pointer = nostr_profile_pointer_new();
  pointer->public_key = strdup(hex);
  pointer->relays = calloc(1, sizeof(char *));
  pointer->relays[0] = strdup(relay);
  pointer->relays_count = 1;
  char *bech = NULL;
  g_assert_cmpint(nostr_nip19_encode_nprofile(pointer, &bech), ==, 0);
  nostr_profile_pointer_free(pointer);
  gchar *copy = g_strdup(bech);
  free(bech);
  return copy;
}

static gchar *
bech_of(int (*encode)(const uint8_t[32], char **), const gchar *hex)
{
  guint8 bytes[32];
  g_assert_true(nostr_hex2bin(bytes, hex, sizeof bytes));
  char *bech = NULL;
  g_assert_cmpint(encode(bytes, &bech), ==, 0);
  gchar *copy = g_strdup(bech);
  free(bech);
  return copy;
}

static void
test_parse_identifiers(void)
{
  const gchar *pubkey_forms[] = { "%s", "nostr:%s", "NOSTR:%s", "@%s", "  %s\n" };
  for (guint i = 0; i < G_N_ELEMENTS(pubkey_forms); i++) {
    g_autofree gchar *text = g_strdup_printf(pubkey_forms[i], npub_bob);
    g_autoptr(GhRecipientInput) input = gh_recipient_input_parse(text);
    g_assert_cmpint(input->kind, ==, GH_RECIPIENT_INPUT_PUBKEY);
    g_assert_cmpstr(input->pubkey, ==, hex_bob);
  }
  g_autofree gchar *upper = g_ascii_strup(npub_bob, -1);
  g_assert_cmpint(kind_of(upper), ==, GH_RECIPIENT_INPUT_PUBKEY);

  /* An nprofile names the person; its relay hint is dropped (P1). */
  g_autofree gchar *nprofile = nprofile_of(hex_bob, "wss://hint.test.invalid");
  g_autofree gchar *nprofile_uri = g_strconcat("nostr:", nprofile, NULL);
  const gchar *profiles[] = { nprofile, nprofile_uri };
  for (guint i = 0; i < G_N_ELEMENTS(profiles); i++) {
    g_autoptr(GhRecipientInput) input = gh_recipient_input_parse(profiles[i]);
    g_assert_cmpint(input->kind, ==, GH_RECIPIENT_INPUT_PUBKEY);
    g_assert_cmpstr(input->pubkey, ==, hex_bob);
    g_assert_null(input->nip05);
  }

  /* Secrets are recognised (and refused by the dialog), valid or not. */
  g_autofree gchar *nsec = bech_of(nostr_nip19_encode_nsec, SK_BOB);
  g_autofree gchar *nsec_uri = g_strconcat("nostr:", nsec, NULL);
  const gchar *secrets[] = { nsec, nsec_uri, "nsec1qqqq", "ncryptsec1qgg9947rlpvqu76pj5ecreduf9jxhselq2nae2kghhvd5g7dgjtcxfqtd67p9m0w57lspw8gsq6yphnm8623nsl8xn9j4jdzz84zm3frztj3z7s35vpzmqf6ksu8r89qk5z2zxfmu5gv8th8wclt0h4p",
                            "bunker://abc?relay=wss://r.test.invalid&secret=x" };
  for (guint i = 0; i < G_N_ELEMENTS(secrets); i++)
    g_assert_cmpint(kind_of(secrets[i]), ==, GH_RECIPIENT_INPUT_SECRET);

  g_autofree gchar *note = bech_of(nostr_nip19_encode_note, hex_bob);
  g_assert_cmpint(kind_of(note), ==, GH_RECIPIENT_INPUT_OTHER_ENTITY);
  g_autofree gchar *note_uri = g_strconcat("nostr:", note, NULL);
  g_assert_cmpint(kind_of(note_uri), ==, GH_RECIPIENT_INPUT_OTHER_ENTITY);

  /* Typos and truncated pastes are invalid, never a different person. */
  g_autofree gchar *truncated = g_strndup(npub_bob, strlen(npub_bob) - 3);
  g_assert_cmpint(kind_of(truncated), ==, GH_RECIPIENT_INPUT_INVALID);
  g_autofree gchar *typo = g_strdup(npub_bob);
  typo[10] = typo[10] == 'q' ? 'p' : 'q';
  g_assert_cmpint(kind_of(typo), ==, GH_RECIPIENT_INPUT_INVALID);
  g_assert_cmpint(kind_of("nostr:hello"), ==, GH_RECIPIENT_INPUT_INVALID);

  g_autoptr(GhRecipientInput) address = gh_recipient_input_parse(" Alice_1.x-y@Sub.Example.COM ");
  g_assert_cmpint(address->kind, ==, GH_RECIPIENT_INPUT_NIP05);
  g_assert_cmpstr(address->nip05, ==, "alice_1.x-y@sub.example.com");
  g_assert_cmpstr(address->nip05_local, ==, "alice_1.x-y");
  g_assert_cmpstr(address->nip05_domain, ==, "sub.example.com");
  g_assert_cmpint(kind_of("_@example.com"), ==, GH_RECIPIENT_INPUT_NIP05);
  g_assert_cmpint(kind_of("xn--bcher-kva@xn--bcher-kva.example"), ==, GH_RECIPIENT_INPUT_NIP05);

  /* Not addresses: IP literals, ports, paths, one label, other characters. */
  const gchar *text[] = {
    "alice", "a@127.0.0.1", "a@localhost", "a@example.com:8443", "a@example.com/x",
    "a b@example.com", "a@-example.com", "a@example.com.", "a@@example.com", "ä@example.com",
    "a@bücher.example", "a@example.123",
  };
  for (guint i = 0; i < G_N_ELEMENTS(text); i++)
    g_assert_cmpint(kind_of(text[i]), ==, GH_RECIPIENT_INPUT_TEXT);
  g_assert_cmpint(kind_of(""), ==, GH_RECIPIENT_INPUT_EMPTY);
  g_assert_cmpint(kind_of(" \t "), ==, GH_RECIPIENT_INPUT_EMPTY);
  g_assert_cmpint(kind_of(NULL), ==, GH_RECIPIENT_INPUT_EMPTY);
}

static void
test_npub_formats(void)
{
  g_autofree gchar *npub = gh_recipient_npub(hex_bob);
  g_assert_cmpstr(npub, ==, npub_bob);
  g_autofree gchar *grouped = gh_recipient_npub_grouped(hex_bob);
  g_auto(GStrv) groups = g_strsplit(grouped, " ", -1);
  g_assert_cmpstr(groups[0], ==, "npub1");
  guint n = g_strv_length(groups);
  for (guint i = 1; i + 1 < n; i++)
    g_assert_cmpuint(strlen(groups[i]), ==, 4);
  g_autofree gchar *joined = g_strjoinv("", groups);
  g_assert_cmpstr(joined, ==, npub_bob);
  g_autofree gchar *shortened = gh_recipient_npub_short(hex_bob);
  g_assert_true(g_str_has_prefix(shortened, "npub1"));
  g_assert_true(g_str_has_suffix(shortened, npub_bob + strlen(npub_bob) - 4));
  g_assert_null(gh_recipient_npub("not hex"));
  g_assert_null(gh_recipient_npub_grouped("abc"));
}

/* ---- NIP-05 rules ------------------------------------------------------------------ */

static void
test_nip05_url_and_document(void)
{
  g_autofree gchar *url = gh_nip05_dup_url("Bob@Example.COM", NULL);
  g_assert_cmpstr(url, ==, "https://example.com/.well-known/nostr.json?name=bob");
  g_autoptr(GError) error = NULL;
  g_assert_null(gh_nip05_dup_url("bob@127.0.0.1", &error));
  g_assert_error(error, GH_NIP05_ERROR, GH_NIP05_ERROR_ADDRESS);
  g_clear_error(&error);

  g_autofree gchar *upper = g_ascii_strup(hex_bob, -1);
  g_autofree gchar *doc = g_strdup_printf(
    "{\"names\":{\"alice\":\"%s\",\"Bob\":\"%s\"},\"relays\":{\"%s\":[\"wss://x.test.invalid\"]}}",
    "00", upper, hex_bob);
  g_autoptr(GBytes) bytes = g_bytes_new(doc, strlen(doc));
  g_autofree gchar *found = gh_nip05_parse_document(bytes, "bob", &error);
  g_assert_no_error(error);
  g_assert_cmpstr(found, ==, hex_bob);

  g_assert_null(gh_nip05_parse_document(bytes, "carol", &error));
  g_assert_error(error, GH_NIP05_ERROR, GH_NIP05_ERROR_NOT_FOUND);
  g_clear_error(&error);
  g_assert_null(gh_nip05_parse_document(bytes, "alice", &error));
  g_assert_error(error, GH_NIP05_ERROR, GH_NIP05_ERROR_RESPONSE);
  g_clear_error(&error);

  const gchar *bad[] = { "", "not json", "[]", "{\"names\":[]}", "{\"names\":{\"bob\":7}}",
                         "{\"relays\":{}}" };
  for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
    g_autoptr(GBytes) body = g_bytes_new(bad[i], strlen(bad[i]));
    g_autofree gchar *none = gh_nip05_parse_document(body, "bob", &error);
    g_assert_null(none);
    g_assert_nonnull(error);
    g_assert_true(error->domain == GH_NIP05_ERROR);
    g_clear_error(&error);
  }
  g_autofree gchar *big = g_strnfill(GH_NIP05_MAX_DOCUMENT + 1, ' ');
  g_autoptr(GBytes) oversized = g_bytes_new(big, strlen(big));
  g_assert_null(gh_nip05_parse_document(oversized, "bob", &error));
  g_assert_error(error, GH_NIP05_ERROR, GH_NIP05_ERROR_RESPONSE);
}

/* ---- consent: a recording transport --------------------------------------------- */

typedef struct {
  GPtrArray *urls;
  GPtrArray *caps;
  GTask *pending;
  gchar *answer;     /* NULL: fail with G_IO_ERROR_HOST_NOT_FOUND */
} Recorder;

static void
rec_get_async(gpointer data, const gchar *uri, gsize max_bytes, GCancellable *cancellable,
              GAsyncReadyCallback callback, gpointer user_data)
{
  Recorder *rec = data;
  g_ptr_array_add(rec->urls, g_strdup(uri));
  g_ptr_array_add(rec->caps, GSIZE_TO_POINTER(max_bytes));
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  if (rec->answer)
    g_task_return_pointer(task, g_bytes_new(rec->answer, strlen(rec->answer)),
                          (GDestroyNotify)g_bytes_unref);
  else
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_HOST_NOT_FOUND, "no such host");
  g_object_unref(task);
}

static GBytes *
rec_get_finish(gpointer data, GAsyncResult *result, GError **error)
{
  (void)data;
  return g_task_propagate_pointer(G_TASK(result), error);
}

static const GhHttpTransport rec_transport = { rec_get_async, rec_get_finish };

typedef struct {
  gboolean done;
  gchar *pubkey;
  GError *error;
} Looked;

static void
on_looked(GObject *source, GAsyncResult *result, gpointer data)
{
  Looked *looked = data;
  looked->pubkey = gh_nip05_lookup_finish(GH_NIP05(source), result, &looked->error);
  looked->done = TRUE;
}

static gboolean
is_done(gpointer data)
{
  return ((Looked *)data)->done;
}

static void
looked_clear(Looked *looked)
{
  g_clear_pointer(&looked->pubkey, g_free);
  g_clear_error(&looked->error);
  looked->done = FALSE;
}

static void
test_nip05_consent(void)
{
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(settings, "network-mode", "system");
  Recorder rec = { g_ptr_array_new_with_free_func(g_free), g_ptr_array_new(), NULL, NULL };
  rec.answer = g_strdup_printf("{\"names\":{\"bob\":\"%s\"}}", hex_bob);
  g_autoptr(GhNip05) nip05 = gh_nip05_new(settings, &rec_transport, &rec);
  /* Parsing, URLs and the lookup object itself contact nothing. */
  g_autoptr(GhRecipientInput) input = gh_recipient_input_parse("bob@example.com");
  g_autofree gchar *url = gh_nip05_dup_url(input->nip05, NULL);
  g_assert_cmpuint(rec.urls->len, ==, 0);

  Looked looked = { 0 };
  gh_nip05_lookup_async(nip05, "Bob@Example.com", NULL, on_looked, &looked);
  spin_until(is_done, &looked);
  g_assert_no_error(looked.error);
  g_assert_cmpstr(looked.pubkey, ==, hex_bob);
  g_assert_cmpuint(rec.urls->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(rec.urls, 0), ==, url);
  g_assert_cmpuint(GPOINTER_TO_SIZE(g_ptr_array_index(rec.caps, 0)), ==, GH_NIP05_MAX_DOCUMENT);
  looked_clear(&looked);

  /* A failed request is reported, not retried. */
  g_clear_pointer(&rec.answer, g_free);
  gh_nip05_lookup_async(nip05, "bob@example.com", NULL, on_looked, &looked);
  spin_until(is_done, &looked);
  g_assert_error(looked.error, G_IO_ERROR, G_IO_ERROR_HOST_NOT_FOUND);
  g_assert_cmpuint(rec.urls->len, ==, 2);
  looked_clear(&looked);

  /* Tor mode (G09): the lookup is handed to the transport (GhNetHttp, which
   * goes through Tor; tests/net), a .onion one over http. Outside Tor a
   * .onion address never reaches the transport (NT-8). */
  g_settings_set_string(settings, "network-mode", "tor");
  gh_nip05_lookup_async(nip05, "bob@example.com", NULL, on_looked, &looked);
  spin_until(is_done, &looked);
  g_assert_cmpuint(rec.urls->len, ==, 3);
  g_assert_cmpstr(g_ptr_array_index(rec.urls, 2), ==,
                  "https://example.com/.well-known/nostr.json?name=bob");
  looked_clear(&looked);
  gh_nip05_lookup_async(nip05, "bob@abcdefghijklmnop.onion", NULL, on_looked, &looked);
  spin_until(is_done, &looked);
  g_assert_cmpuint(rec.urls->len, ==, 4);
  g_assert_cmpstr(g_ptr_array_index(rec.urls, 3), ==,
                  "http://abcdefghijklmnop.onion/.well-known/nostr.json?name=bob");
  looked_clear(&looked);
  g_settings_set_string(settings, "network-mode", "none");
  gh_nip05_lookup_async(nip05, "bob@abcdefghijklmnop.onion", NULL, on_looked, &looked);
  spin_until(is_done, &looked);
  g_assert_error(looked.error, GH_NIP05_ERROR, GH_NIP05_ERROR_NETWORK_MODE);
  looked_clear(&looked);
  gh_nip05_lookup_async(nip05, "not an address", NULL, on_looked, &looked);
  spin_until(is_done, &looked);
  g_assert_error(looked.error, GH_NIP05_ERROR, GH_NIP05_ERROR_ADDRESS);
  looked_clear(&looked);
  g_assert_cmpuint(rec.urls->len, ==, 4);
  g_settings_reset(settings, "network-mode");
  g_ptr_array_unref(rec.urls);
  g_ptr_array_unref(rec.caps);
}

/* ---- GhNetHttp against a loopback server ----------------------------------------- */

typedef struct {
  SoupServer *server;
  gchar *base; /* "http://127.0.0.1:<port>" */
  guint hits_doc;
  guint hits_target;
  GPtrArray *headers; /* "name: value" of the last request */
} Server;

static void
record_headers(Server *server, SoupServerMessage *msg)
{
  g_ptr_array_set_size(server->headers, 0);
  SoupMessageHeadersIter iter;
  const char *name, *value;
  soup_message_headers_iter_init(&iter, soup_server_message_get_request_headers(msg));
  while (soup_message_headers_iter_next(&iter, &name, &value))
    g_ptr_array_add(server->headers, g_strdup_printf("%s: %s", name, value));
}

static gboolean
sent_header(Server *server, const gchar *name)
{
  for (guint i = 0; i < server->headers->len; i++) {
    const gchar *line = g_ptr_array_index(server->headers, i);
    if (g_ascii_strncasecmp(line, name, strlen(name)) == 0 && line[strlen(name)] == ':')
      return TRUE;
  }
  return FALSE;
}

static void
on_request(SoupServer *soup, SoupServerMessage *msg, const char *path, GHashTable *query,
           gpointer data)
{
  Server *server = data;
  (void)soup;
  (void)query;
  record_headers(server, msg);
  if (g_str_equal(path, "/.well-known/nostr.json")) {
    server->hits_doc++;
    g_autofree gchar *doc = g_strdup_printf("{\"names\":{\"bob\":\"%s\"}}", hex_bob);
    soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
    /* A server may try to set a cookie; the client keeps no jar. */
    soup_message_headers_append(soup_server_message_get_response_headers(msg), "Set-Cookie",
                                "track=1");
    soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_COPY, doc,
                                     strlen(doc));
  } else if (g_str_equal(path, "/redirect")) {
    soup_server_message_set_redirect(msg, SOUP_STATUS_FOUND, "/target");
  } else if (g_str_equal(path, "/target")) {
    server->hits_target++;
    soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
    soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_STATIC, "{}", 2);
  } else if (g_str_equal(path, "/big")) {
    g_autofree gchar *big = g_strnfill(70 * 1024, 'x');
    soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
    soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_COPY, big,
                                     strlen(big));
  } else {
    soup_server_message_set_status(msg, SOUP_STATUS_NOT_FOUND, NULL);
  }
}

static void
server_up(Server *server)
{
  memset(server, 0, sizeof *server);
  server->headers = g_ptr_array_new_with_free_func(g_free);
  server->server = soup_server_new(NULL, NULL);
  soup_server_add_handler(server->server, NULL, on_request, server, NULL);
  g_autoptr(GError) error = NULL;
  g_assert_true(soup_server_listen_local(server->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY,
                                         &error));
  g_assert_no_error(error);
  GSList *uris = soup_server_get_uris(server->server);
  g_assert_nonnull(uris);
  server->base = g_strdup_printf("http://127.0.0.1:%d", g_uri_get_port(uris->data));
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
}

static void
server_down(Server *server)
{
  soup_server_disconnect(server->server);
  g_object_unref(server->server);
  g_free(server->base);
  g_ptr_array_unref(server->headers);
}

typedef struct {
  gboolean done;
  GBytes *body;
  GError *error;
} Got;

static void
on_got(GObject *source, GAsyncResult *result, gpointer data)
{
  Got *got = data;
  got->body = gh_net_http_get_finish(GH_NET_HTTP(source), result, &got->error);
  got->done = TRUE;
}

static gboolean
got_done(gpointer data)
{
  return ((Got *)data)->done;
}

static void
get(GhNetHttp *http, const gchar *uri, gsize cap, Got *got)
{
  g_clear_pointer(&got->body, g_bytes_unref);
  g_clear_error(&got->error);
  got->done = FALSE;
  gh_net_http_get_async(http, uri, cap, NULL, on_got, got);
  spin_until(got_done, got);
}

static void
test_net_http(void)
{
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(settings, "network-mode", "none");
  Server server;
  server_up(&server);
  g_autoptr(GhNetHttp) http = gh_net_http_new(settings);
  Got got = { 0 };

  g_autofree gchar *doc = g_strconcat(server.base, "/.well-known/nostr.json?name=bob", NULL);
  get(http, doc, GH_NIP05_MAX_DOCUMENT, &got);
  g_assert_no_error(got.error);
  g_autofree gchar *pubkey = gh_nip05_parse_document(got.body, "bob", NULL);
  g_assert_cmpstr(pubkey, ==, hex_bob);
  g_assert_cmpuint(server.hits_doc, ==, 1);
  /* Nothing that identifies the client or links requests. */
  g_assert_false(sent_header(&server, "User-Agent"));
  g_assert_false(sent_header(&server, "Referer"));
  g_assert_false(sent_header(&server, "Accept-Language"));
  g_assert_false(sent_header(&server, "Authorization"));
  g_assert_true(sent_header(&server, "Accept"));
  get(http, doc, GH_NIP05_MAX_DOCUMENT, &got);
  g_assert_no_error(got.error);
  g_assert_false(sent_header(&server, "Cookie")); /* no cookie jar */

  /* A redirect is an error, and its target is never fetched (NIP-05). */
  g_autofree gchar *redirect = g_strconcat(server.base, "/redirect", NULL);
  get(http, redirect, GH_NIP05_MAX_DOCUMENT, &got);
  g_assert_error(got.error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_assert_cmpuint(server.hits_target, ==, 0);

  g_autofree gchar *missing = g_strconcat(server.base, "/missing", NULL);
  get(http, missing, GH_NIP05_MAX_DOCUMENT, &got);
  g_assert_error(got.error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);

  g_autofree gchar *big = g_strconcat(server.base, "/big", NULL);
  get(http, big, GH_NIP05_MAX_DOCUMENT, &got);
  g_assert_error(got.error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE);
  get(http, big, 80 * 1024, &got);
  g_assert_no_error(got.error);
  g_assert_cmpuint(g_bytes_get_size(got.body), ==, 70 * 1024);

  /* https only; plain http only to loopback (these fixtures). */
  const gchar *refused[] = { "http://example.com/.well-known/nostr.json",
                             "ftp://example.com/x", "https://user@example.com/x", "not a uri" };
  for (guint i = 0; i < G_N_ELEMENTS(refused); i++) {
    get(http, refused[i], 1024, &got);
    g_assert_error(got.error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  }
  /* Tor mode with nothing at the Tor address: an error, never a direct
   * connection (G09; the Tor path itself is tests/net). A .onion host only
   * in Tor mode. */
  guint hits = server.hits_doc;
  g_autoptr(GSocketListener) reservation = g_socket_listener_new();
  guint16 closed_port = g_socket_listener_add_any_inet_port(reservation, NULL, NULL);
  g_socket_listener_close(reservation);
  g_autofree gchar *nowhere = g_strdup_printf("127.0.0.1:%u", closed_port);
  g_settings_set_string(settings, "tor-socks-address", nowhere);
  g_settings_set_string(settings, "network-mode", "tor");
  get(http, doc, GH_NIP05_MAX_DOCUMENT, &got);
  g_assert_nonnull(got.error);
  g_assert_cmpuint(server.hits_doc, ==, hits);
  g_settings_reset(settings, "tor-socks-address");
  g_settings_set_string(settings, "network-mode", "none");
  get(http, "https://abcdefghijklmnop.onion/.well-known/nostr.json", 1024, &got);
  g_assert_error(got.error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
  /* The desktop proxy settings (system mode) reach loopback directly. */
  g_settings_set_string(settings, "network-mode", "system");
  get(http, doc, GH_NIP05_MAX_DOCUMENT, &got);
  g_assert_no_error(got.error);
  g_assert_cmpuint(server.hits_doc, ==, hits + 1);

  g_clear_pointer(&got.body, g_bytes_unref);
  g_clear_error(&got.error);
  g_settings_reset(settings, "network-mode");
  g_clear_object(&http);
  server_down(&server);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  /* The loopback fixtures must not go through a host proxy. */
  g_setenv("no_proxy", "127.0.0.1,localhost", TRUE);
  g_setenv("GIO_USE_PROXY_RESOLVER", "dummy", TRUE);
  char *pub = nostr_key_get_public(SK_BOB);
  hex_bob = g_strdup(pub);
  free(pub);
  npub_bob = gh_recipient_npub(hex_bob);
  g_test_add_func("/groundhog/new-message/parse-identifiers", test_parse_identifiers);
  g_test_add_func("/groundhog/new-message/npub-formats", test_npub_formats);
  g_test_add_func("/groundhog/new-message/nip05-url-and-document", test_nip05_url_and_document);
  g_test_add_func("/groundhog/new-message/nip05-consent", test_nip05_consent);
  g_test_add_func("/groundhog/new-message/net-http", test_net_http);
  int status = g_test_run();
  g_free(hex_bob);
  g_free(npub_bob);
  return status;
}
