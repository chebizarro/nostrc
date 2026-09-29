#include "gh-nip29-group.h"
#include "gh-nip29-template.h"

#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-kinds.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define RELAY_A "wss://groups.example.org"
#define RELAY_B "wss://relay.example.net"
#define GROUP_ID "pizza"

typedef struct {
  gchar *sk;
  gchar *pk;
} TestKey;

/* Generated per run; nothing here is a real identity. */
static TestKey relay_a, relay_b, attacker, owner, moderator, gardener, member, stranger;

static void
key_init(TestKey *key)
{
  char *sk = nostr_key_generate_private();
  g_assert_nonnull(sk);
  char *pk = nostr_key_get_public(sk);
  g_assert_true(gh_nip29_is_hex64(pk));
  key->sk = g_strdup(sk);
  key->pk = g_strdup(pk);
  free(sk);
  free(pk);
}

static void
key_clear(TestKey *key)
{
  g_free(key->sk);
  g_free(key->pk);
}

static NostrEvent *
event_new(gint kind, gint64 created_at, const gchar *d)
{
  NostrEvent *event = nostr_event_new();
  g_assert_nonnull(event);
  nostr_event_set_kind(event, kind);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, "");
  if (d) {
    NostrTags *tags = nostr_event_get_tags(event);
    nostr_tags_append(tags, nostr_tag_new("d", d, NULL));
  }
  return event;
}

/* Appends [key, values..., NULL]. */
static void
event_tag(NostrEvent *event, const gchar *key, ...)
{
  NostrTag *tag = nostr_tag_new(key, NULL);
  va_list args;
  va_start(args, key);
  for (const gchar *value = va_arg(args, const gchar *); value;
       value = va_arg(args, const gchar *))
    nostr_tag_append(tag, value);
  va_end(args);
  nostr_tags_append(nostr_event_get_tags(event), tag);
}

static NostrEvent *
sign(const TestKey *key, NostrEvent *event)
{
  g_assert_cmpint(nostr_event_sign(event, key->sk), ==, 0);
  g_assert_true(nostr_event_check_signature(event));
  return event;
}

static NostrEvent *
metadata_event(const TestKey *key, gint64 created_at, const gchar *name)
{
  NostrEvent *event = event_new(NOSTR_KIND_SIMPLE_GROUP_METADATA, created_at, GROUP_ID);
  event_tag(event, "name", name, NULL);
  return sign(key, event);
}

static GhNip29Group *
group_new(const gchar *relay, const TestKey *relay_key)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip29GroupKey) key = gh_nip29_group_key_new(relay, GROUP_ID, &error);
  g_assert_no_error(error);
  GhNip29Group *group = gh_nip29_group_new(key, relay_key->pk, &error);
  g_assert_no_error(error);
  return group;
}

static GhNip29Admission
admit_free(GhNip29Group *group, NostrEvent *event)
{
  GhNip29Admission admission = gh_nip29_group_admit(group, event);
  nostr_event_free(event);
  return admission;
}

static gboolean
strv_equal(const gchar *const *strv, const gchar *const *expected)
{
  return g_strv_equal(strv ? strv : (const gchar *const[]){ NULL }, expected);
}

/* ---- Identity ------------------------------------------------------------ */

static void
test_relay_url_normalization(void)
{
  static const struct {
    const gchar *in;
    const gchar *out;
  } valid[] = {
    { "WSS://Groups.Example.ORG:443/", "wss://groups.example.org" },
    { "  wss://groups.example.org  ", "wss://groups.example.org" },
    { "ws://relay.local:80", "ws://relay.local" },
    { "wss://relay.local:444/", "wss://relay.local:444" },
    { "wss://relay.local/Groups//", "wss://relay.local/Groups" },
    { "ws://[::1]:7777", "ws://[::1]:7777" },
    /* Equivalent percent-encodings are one endpoint. */
    { "wss://relay.local/%7ealice/", "wss://relay.local/~alice" },
    { "wss://relay.local/%7Ealice", "wss://relay.local/~alice" },
    { "wss://relay.local/~alice", "wss://relay.local/~alice" },
    { "wss://relay.local/a%2fb", "wss://relay.local/a%2Fb" },
    /* IDN hosts compare in punycode. */
    { "wss://B\xc3\xbc" "cher.example", "wss://xn--bcher-kva.example" },
    { "wss://xn--bcher-kva.example/", "wss://xn--bcher-kva.example" },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(valid); i++) {
    g_autoptr(GError) error = NULL;
    g_autofree gchar *out = gh_nip29_normalize_relay_url(valid[i].in, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(out, ==, valid[i].out);
  }

  static const gchar *const invalid[] = {
    "https://groups.example.org", "wss://user@groups.example.org",
    "wss://groups.example.org/?x=1", "wss://groups.example.org/#top",
    "groups.example.org", "", "wss://", "wss://groups.example.org:0",
    "wss://relay.local/%zz", "wss://relay.local/%4",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(invalid); i++) {
    g_autoptr(GError) error = NULL;
    g_autofree gchar *out = gh_nip29_normalize_relay_url(invalid[i], &error);
    g_assert_null(out);
    g_assert_error(error, GH_NIP29_ERROR, GH_NIP29_ERROR_INVALID_RELAY_URL);
  }
  g_autoptr(GError) error = NULL;
  g_assert_null(gh_nip29_normalize_relay_url(NULL, &error));
  g_assert_error(error, GH_NIP29_ERROR, GH_NIP29_ERROR_INVALID_RELAY_URL);
}

static void
test_group_key_identity(void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip29GroupKey) a = gh_nip29_group_key_new("wss://groups.example.org/", GROUP_ID, &error);
  g_assert_no_error(error);
  g_autoptr(GhNip29GroupKey) a_alias = gh_nip29_group_key_new("WSS://GROUPS.example.org:443", GROUP_ID, &error);
  g_assert_no_error(error);
  g_autoptr(GhNip29GroupKey) b = gh_nip29_group_key_new(RELAY_B, GROUP_ID, &error);
  g_assert_no_error(error);
  g_autoptr(GhNip29GroupKey) other_id = gh_nip29_group_key_new(RELAY_A, "pasta", &error);
  g_assert_no_error(error);

  g_assert_cmpstr(gh_nip29_group_key_get_relay_url(a), ==, RELAY_A);
  g_assert_cmpstr(gh_nip29_group_key_get_group_id(a), ==, GROUP_ID);
  g_assert_true(gh_nip29_group_key_equal(a, a_alias));
  g_assert_cmpuint(gh_nip29_group_key_hash(a), ==, gh_nip29_group_key_hash(a_alias));
  g_assert_false(gh_nip29_group_key_equal(a, b));
  g_assert_false(gh_nip29_group_key_equal(a, other_id));

  g_autoptr(GHashTable) groups = g_hash_table_new_full(
    gh_nip29_group_key_hash, gh_nip29_group_key_equal,
    (GDestroyNotify)gh_nip29_group_key_free, NULL);
  g_hash_table_add(groups, gh_nip29_group_key_copy(a));
  g_hash_table_add(groups, gh_nip29_group_key_copy(b));
  g_hash_table_add(groups, gh_nip29_group_key_copy(a_alias));
  g_assert_cmpuint(g_hash_table_size(groups), ==, 2);

  static const gchar *const bad_ids[] = { "Pizza", "", "piz za", "pizza'1" };
  for (gsize i = 0; i < G_N_ELEMENTS(bad_ids); i++) {
    g_autoptr(GError) id_error = NULL;
    g_assert_null(gh_nip29_group_key_new(RELAY_A, bad_ids[i], &id_error));
    g_assert_error(id_error, GH_NIP29_ERROR, GH_NIP29_ERROR_INVALID_GROUP_ID);
  }
  g_clear_error(&error);
  g_assert_null(gh_nip29_group_key_new(RELAY_A, NULL, &error));
  g_assert_error(error, GH_NIP29_ERROR, GH_NIP29_ERROR_INVALID_GROUP_ID);
  g_clear_error(&error);
  g_assert_null(gh_nip29_group_key_new("https://groups.example.org", GROUP_ID, &error));
  g_assert_error(error, GH_NIP29_ERROR, GH_NIP29_ERROR_INVALID_RELAY_URL);

  g_clear_error(&error);
  g_assert_null(gh_nip29_group_new(a, "not-a-key", &error));
  g_assert_error(error, GH_NIP29_ERROR, GH_NIP29_ERROR_INVALID_PUBKEY);
  g_clear_error(&error);
  g_autofree gchar *upper = g_ascii_strup(relay_a.pk, -1);
  g_autoptr(GhNip29Group) group = gh_nip29_group_new(a, upper, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(gh_nip29_group_get_relay_pubkey(group), ==, relay_a.pk);
  g_assert_true(gh_nip29_group_key_equal(gh_nip29_group_get_key(group), a));
}

static void
test_same_id_on_two_relays_is_isolated(void)
{
  g_autoptr(GhNip29Group) on_a = group_new(RELAY_A, &relay_a);
  g_autoptr(GhNip29Group) on_b = group_new(RELAY_B, &relay_b);
  g_assert_false(gh_nip29_group_key_equal(gh_nip29_group_get_key(on_a),
                                          gh_nip29_group_get_key(on_b)));

  NostrEvent *meta_a = event_new(NOSTR_KIND_SIMPLE_GROUP_METADATA, 100, GROUP_ID);
  event_tag(meta_a, "name", "A pizza", NULL);
  event_tag(meta_a, "closed", NULL);
  sign(&relay_a, meta_a);
  NostrEvent *meta_b = metadata_event(&relay_b, 100, "B pizza");

  /* Each relay's snapshot is foreign to the other group with the same id. */
  g_assert_cmpint(gh_nip29_group_admit(on_b, meta_a), ==, GH_NIP29_ADMISSION_WRONG_AUTHOR);
  g_assert_cmpint(gh_nip29_group_admit(on_a, meta_b), ==, GH_NIP29_ADMISSION_WRONG_AUTHOR);
  g_assert_cmpint(admit_free(on_a, meta_a), ==, GH_NIP29_ADMISSION_ACCEPTED);
  g_assert_cmpint(admit_free(on_b, meta_b), ==, GH_NIP29_ADMISSION_ACCEPTED);

  g_autoptr(GhNip29Metadata) md_a = gh_nip29_group_dup_metadata(on_a);
  g_autoptr(GhNip29Metadata) md_b = gh_nip29_group_dup_metadata(on_b);
  g_assert_cmpstr(md_a->name, ==, "A pizza");
  g_assert_true(md_a->is_closed);
  g_assert_cmpstr(md_b->name, ==, "B pizza");
  g_assert_false(md_b->is_closed);

  NostrEvent *admins_a = event_new(NOSTR_KIND_SIMPLE_GROUP_ADMINS, 100, GROUP_ID);
  event_tag(admins_a, "p", owner.pk, "ceo", NULL);
  sign(&relay_a, admins_a);
  g_assert_cmpint(gh_nip29_group_admit(on_b, admins_a), ==, GH_NIP29_ADMISSION_WRONG_AUTHOR);
  g_assert_cmpint(admit_free(on_a, admins_a), ==, GH_NIP29_ADMISSION_ACCEPTED);

  g_auto(GStrv) roles_a = gh_nip29_group_dup_admin_roles(on_a, owner.pk);
  g_assert_true(strv_equal((const gchar *const *)roles_a, (const gchar *const[]){ "ceo", NULL }));
  g_assert_null(gh_nip29_group_dup_admin_roles(on_b, owner.pk));
  g_assert_cmpint(gh_nip29_group_check_permission(on_b, NULL, owner.pk, NOSTR_PERMISSION_PUT_USER),
                  ==, GH_NIP29_AUTHZ_UNKNOWN_NO_ADMINS);
}

/* ---- Admission ----------------------------------------------------------- */

static void
test_rejects_unverified_snapshots(void)
{
  g_autoptr(GhNip29Group) group = group_new(RELAY_A, &relay_a);

  /* Unsigned: right author and d, but no id/signature. */
  NostrEvent *unsigned_event = event_new(NOSTR_KIND_SIMPLE_GROUP_METADATA, 100, GROUP_ID);
  nostr_event_set_pubkey(unsigned_event, relay_a.pk);
  g_assert_cmpint(gh_nip29_group_admit(group, unsigned_event), ==, GH_NIP29_ADMISSION_UNSIGNED);
  /* A canonical id alone is still unsigned. libnostr frees id with free(). */
  unsigned_event->id = nostr_event_get_id(unsigned_event);
  g_assert_true(gh_nip29_is_hex64(unsigned_event->id));
  g_assert_cmpint(admit_free(group, unsigned_event), ==, GH_NIP29_ADMISSION_UNSIGNED);

  /* Forged: content changed after the relay signed. */
  NostrEvent *tampered = metadata_event(&relay_a, 100, "real");
  nostr_event_set_content(tampered, "tampered");
  g_assert_cmpint(admit_free(group, tampered), ==, GH_NIP29_ADMISSION_FORGED);

  /* Forged: attacker-signed event relabelled with the relay pubkey. */
  NostrEvent *relabelled = metadata_event(&attacker, 100, "relabelled");
  nostr_event_set_pubkey(relabelled, relay_a.pk);
  g_assert_cmpint(admit_free(group, relabelled), ==, GH_NIP29_ADMISSION_FORGED);

  /* Forged: canonical id, but a relay signature lifted from another event. */
  NostrEvent *donor = metadata_event(&relay_a, 100, "donor");
  NostrEvent *lifted = metadata_event(&relay_a, 100, "lifted");
  nostr_event_set_sig(lifted, nostr_event_get_sig(donor));
  nostr_event_free(donor);
  g_assert_cmpint(admit_free(group, lifted), ==, GH_NIP29_ADMISSION_FORGED);

  /* Validly signed by a key that is not the relay's NIP-11 self. */
  g_assert_cmpint(admit_free(group, metadata_event(&attacker, 100, "impostor")), ==,
                  GH_NIP29_ADMISSION_WRONG_AUTHOR);
  g_assert_cmpint(admit_free(group, metadata_event(&relay_b, 100, "other relay")), ==,
                  GH_NIP29_ADMISSION_WRONG_AUTHOR);

  /* Relay-signed, but for another group, no group, or a wrong first d. */
  NostrEvent *other_group = event_new(NOSTR_KIND_SIMPLE_GROUP_METADATA, 100, "pasta");
  g_assert_cmpint(admit_free(group, sign(&relay_a, other_group)), ==, GH_NIP29_ADMISSION_WRONG_GROUP);
  NostrEvent *no_d = event_new(NOSTR_KIND_SIMPLE_GROUP_METADATA, 100, NULL);
  g_assert_cmpint(admit_free(group, sign(&relay_a, no_d)), ==, GH_NIP29_ADMISSION_WRONG_GROUP);
  NostrEvent *second_d = event_new(NOSTR_KIND_SIMPLE_GROUP_METADATA, 100, "pasta");
  event_tag(second_d, "d", GROUP_ID, NULL);
  g_assert_cmpint(admit_free(group, sign(&relay_a, second_d)), ==, GH_NIP29_ADMISSION_WRONG_GROUP);

  /* Relay-signed but outside 39000-39003. */
  g_assert_cmpint(admit_free(group, sign(&relay_a, event_new(NOSTR_KIND_SIMPLE_GROUP_PINNED_EVENTS, 100, GROUP_ID))),
                  ==, GH_NIP29_ADMISSION_UNSUPPORTED_KIND);
  NostrEvent *chat = event_new(NOSTR_KIND_SIMPLE_GROUP_CHAT_MESSAGE, 100, NULL);
  event_tag(chat, "h", GROUP_ID, NULL);
  g_assert_cmpint(admit_free(group, sign(&relay_a, chat)), ==, GH_NIP29_ADMISSION_UNSUPPORTED_KIND);

  /* Structurally invalid id encoding. */
  NostrEvent *upper_id = metadata_event(&relay_a, 100, "upper");
  for (gchar *p = upper_id->id; *p; p++)
    *p = g_ascii_toupper(*p);
  g_assert_cmpint(admit_free(group, upper_id), ==, GH_NIP29_ADMISSION_MALFORMED);
  g_assert_cmpint(gh_nip29_group_admit(group, NULL), ==, GH_NIP29_ADMISSION_MALFORMED);

  g_assert_null(gh_nip29_group_dup_metadata(group));
  g_assert_null(gh_nip29_group_get_snapshot_id(group, NOSTR_KIND_SIMPLE_GROUP_METADATA, NULL));
  for (gint a = GH_NIP29_ADMISSION_ACCEPTED; a <= GH_NIP29_ADMISSION_FAILED; a++)
    g_assert_true(*gh_nip29_admission_to_string((GhNip29Admission)a) != '\0');
}

static void
test_newest_wins_and_tie_break(void)
{
  g_autoptr(GhNip29Group) group = group_new(RELAY_A, &relay_a);
  NostrEvent *newer = metadata_event(&relay_a, 200, "newer");
  NostrEvent *older = metadata_event(&relay_a, 100, "older");
  g_assert_cmpint(gh_nip29_group_admit(group, newer), ==, GH_NIP29_ADMISSION_ACCEPTED);
  g_assert_cmpint(admit_free(group, older), ==, GH_NIP29_ADMISSION_STALE);
  g_assert_cmpint(gh_nip29_group_admit(group, newer), ==, GH_NIP29_ADMISSION_DUPLICATE);
  {
    g_autoptr(GhNip29Metadata) md = gh_nip29_group_dup_metadata(group);
    g_assert_cmpstr(md->name, ==, "newer");
  }
  gint64 created_at = 0;
  g_assert_cmpstr(gh_nip29_group_get_snapshot_id(group, NOSTR_KIND_SIMPLE_GROUP_METADATA, &created_at),
                  ==, newer->id);
  g_assert_cmpint(created_at, ==, 200);
  nostr_event_free(newer);

  /* Equal created_at: the lexically lowest id wins in either arrival order. */
  NostrEvent *one = metadata_event(&relay_a, 300, "tie-one");
  NostrEvent *two = metadata_event(&relay_a, 300, "tie-two");
  gboolean one_is_low = strcmp(one->id, two->id) < 0;
  NostrEvent *low = one_is_low ? one : two;
  NostrEvent *high = one_is_low ? two : one;
  const gchar *low_name = one_is_low ? "tie-one" : "tie-two";

  g_autoptr(GhNip29Group) forward = group_new(RELAY_A, &relay_a);
  g_assert_cmpint(gh_nip29_group_admit(forward, high), ==, GH_NIP29_ADMISSION_ACCEPTED);
  g_assert_cmpint(gh_nip29_group_admit(forward, low), ==, GH_NIP29_ADMISSION_ACCEPTED);
  g_assert_cmpint(gh_nip29_group_admit(forward, high), ==, GH_NIP29_ADMISSION_STALE);
  g_autoptr(GhNip29Group) reverse = group_new(RELAY_A, &relay_a);
  g_assert_cmpint(gh_nip29_group_admit(reverse, low), ==, GH_NIP29_ADMISSION_ACCEPTED);
  g_assert_cmpint(gh_nip29_group_admit(reverse, high), ==, GH_NIP29_ADMISSION_STALE);
  GhNip29Group *both[] = { forward, reverse };
  for (gsize i = 0; i < G_N_ELEMENTS(both); i++) {
    g_autoptr(GhNip29Metadata) md = gh_nip29_group_dup_metadata(both[i]);
    g_assert_cmpstr(md->name, ==, low_name);
    g_assert_cmpstr(gh_nip29_group_get_snapshot_id(both[i], NOSTR_KIND_SIMPLE_GROUP_METADATA, NULL),
                    ==, low->id);
  }
  g_assert_cmpint(gh_nip29_group_admit(group, high), ==, GH_NIP29_ADMISSION_ACCEPTED);
  nostr_event_free(one);
  nostr_event_free(two);

  /* Each kind keeps its own clock. */
  NostrEvent *admins = event_new(NOSTR_KIND_SIMPLE_GROUP_ADMINS, 50, GROUP_ID);
  event_tag(admins, "p", owner.pk, "ceo", NULL);
  g_assert_cmpint(admit_free(group, sign(&relay_a, admins)), ==, GH_NIP29_ADMISSION_ACCEPTED);
  NostrEvent *stale_admins = event_new(NOSTR_KIND_SIMPLE_GROUP_ADMINS, 49, GROUP_ID);
  event_tag(stale_admins, "p", attacker.pk, "ceo", NULL);
  g_assert_cmpint(admit_free(group, sign(&relay_a, stale_admins)), ==, GH_NIP29_ADMISSION_STALE);
  g_assert_null(gh_nip29_group_dup_admin_roles(group, attacker.pk));
}

/* ---- Queries ------------------------------------------------------------- */

static void
test_admin_authorization(void)
{
  g_autoptr(GhNip29Group) group = group_new(RELAY_A, &relay_a);
  g_autoptr(GhNip29RolePolicy) policy = gh_nip29_role_policy_new();
  g_autoptr(GError) error = NULL;
  const nostr_permission_t all[] = {
    NOSTR_PERMISSION_PUT_USER, NOSTR_PERMISSION_REMOVE_USER, NOSTR_PERMISSION_EDIT_METADATA,
    NOSTR_PERMISSION_DELETE_EVENT, NOSTR_PERMISSION_CREATE_INVITE,
  };
  const nostr_permission_t moderate[] = { NOSTR_PERMISSION_DELETE_EVENT };
  g_assert_true(gh_nip29_role_policy_set_role(policy, "ceo", all, G_N_ELEMENTS(all), &error));
  g_assert_true(gh_nip29_role_policy_set_role(policy, "moderator", moderate, 1, &error));
  g_assert_true(gh_nip29_role_policy_set_role(policy, "gardener", all, G_N_ELEMENTS(all), &error));
  g_assert_no_error(error);
  g_assert_false(gh_nip29_role_policy_set_role(policy, "", all, 1, &error));
  g_assert_error(error, GH_NIP29_ERROR, GH_NIP29_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);
  const nostr_permission_t bogus[] = { (nostr_permission_t)42 };
  g_assert_false(gh_nip29_role_policy_set_role(policy, "ceo", bogus, 1, &error));
  g_assert_error(error, GH_NIP29_ERROR, GH_NIP29_ERROR_INVALID_ARGUMENT);

  /* No relay-signed admin list yet: unknown, therefore denied. */
  g_assert_cmpint(gh_nip29_group_check_permission(group, policy, owner.pk, NOSTR_PERMISSION_PUT_USER),
                  ==, GH_NIP29_AUTHZ_UNKNOWN_NO_ADMINS);
  g_assert_false(gh_nip29_group_can(group, policy, owner.pk, NOSTR_PERMISSION_PUT_USER));
  g_assert_null(gh_nip29_group_dup_admins(group));

  NostrEvent *forged = event_new(NOSTR_KIND_SIMPLE_GROUP_ADMINS, 100, GROUP_ID);
  event_tag(forged, "p", attacker.pk, "ceo", NULL);
  g_assert_cmpint(admit_free(group, sign(&attacker, forged)), ==, GH_NIP29_ADMISSION_WRONG_AUTHOR);

  NostrEvent *admins = event_new(NOSTR_KIND_SIMPLE_GROUP_ADMINS, 100, GROUP_ID);
  event_tag(admins, "p", owner.pk, "ceo", NULL);
  event_tag(admins, "p", moderator.pk, "moderator", NULL);
  event_tag(admins, "p", gardener.pk, "gardener", NULL);
  event_tag(admins, "p", "not-a-pubkey", "ceo", NULL);
  g_assert_cmpint(admit_free(group, sign(&relay_a, admins)), ==, GH_NIP29_ADMISSION_ACCEPTED);

  g_assert_cmpint(gh_nip29_group_check_permission(group, policy, owner.pk, NOSTR_PERMISSION_PUT_USER),
                  ==, GH_NIP29_AUTHZ_ALLOWED);
  g_assert_true(gh_nip29_group_can(group, policy, owner.pk, NOSTR_PERMISSION_EDIT_METADATA));
  g_assert_true(gh_nip29_group_can(group, policy, moderator.pk, NOSTR_PERMISSION_DELETE_EVENT));
  g_assert_cmpint(gh_nip29_group_check_permission(group, policy, moderator.pk, NOSTR_PERMISSION_REMOVE_USER),
                  ==, GH_NIP29_AUTHZ_DENIED_BY_POLICY);
  g_assert_false(gh_nip29_group_can(group, policy, moderator.pk, NOSTR_PERMISSION_REMOVE_USER));
  g_assert_cmpint(gh_nip29_group_check_permission(group, policy, member.pk, NOSTR_PERMISSION_DELETE_EVENT),
                  ==, GH_NIP29_AUTHZ_DENIED_NOT_ADMIN);
  g_assert_cmpint(gh_nip29_group_check_permission(group, policy, attacker.pk, NOSTR_PERMISSION_PUT_USER),
                  ==, GH_NIP29_AUTHZ_DENIED_NOT_ADMIN);
  /* An admin whose role policy is unknown is not presumed capable. */
  g_assert_cmpint(gh_nip29_group_check_permission(group, NULL, owner.pk, NOSTR_PERMISSION_PUT_USER),
                  ==, GH_NIP29_AUTHZ_UNKNOWN_POLICY);
  g_assert_false(gh_nip29_group_can(group, NULL, owner.pk, NOSTR_PERMISSION_PUT_USER));
  g_assert_cmpint(gh_nip29_group_check_permission(group, policy, "not-a-pubkey", NOSTR_PERMISSION_PUT_USER),
                  ==, GH_NIP29_AUTHZ_DENIED_INVALID);
  g_assert_cmpint(gh_nip29_group_check_permission(group, policy, owner.pk, (nostr_permission_t)42),
                  ==, GH_NIP29_AUTHZ_DENIED_INVALID);
  g_assert_true(gh_nip29_group_can(group, policy, gardener.pk, NOSTR_PERMISSION_PUT_USER));

  /* Once the relay advertises its roles, a role it does not list carries no
   * privilege even if the caller's policy thinks otherwise. */
  NostrEvent *roles = event_new(NOSTR_KIND_SIMPLE_GROUP_ROLES, 100, GROUP_ID);
  event_tag(roles, "role", "ceo", "Owns the group", NULL);
  event_tag(roles, "role", "moderator", NULL);
  g_assert_cmpint(admit_free(group, sign(&relay_a, roles)), ==, GH_NIP29_ADMISSION_ACCEPTED);
  g_assert_cmpint(gh_nip29_group_check_permission(group, policy, gardener.pk, NOSTR_PERMISSION_PUT_USER),
                  ==, GH_NIP29_AUTHZ_DENIED_UNADVERTISED_ROLES);
  g_assert_true(gh_nip29_group_can(group, policy, owner.pk, NOSTR_PERMISSION_PUT_USER));

  g_autoptr(GPtrArray) listed = gh_nip29_group_dup_admins(group);
  g_assert_cmpuint(listed->len, ==, 3);
  GhNip29Admin *first = g_ptr_array_index(listed, 0);
  g_assert_cmpstr(first->pubkey, ==, owner.pk);
  g_assert_true(strv_equal((const gchar *const *)first->roles, (const gchar *const[]){ "ceo", NULL }));
  g_assert_null(gh_nip29_group_dup_admin_roles(group, stranger.pk));

  g_autoptr(GPtrArray) advertised = gh_nip29_group_dup_roles(group);
  g_assert_cmpuint(advertised->len, ==, 2);
  GhNip29Role *ceo = g_ptr_array_index(advertised, 0);
  GhNip29Role *mod = g_ptr_array_index(advertised, 1);
  g_assert_cmpstr(ceo->name, ==, "ceo");
  g_assert_cmpstr(ceo->description, ==, "Owns the group");
  g_assert_cmpstr(mod->name, ==, "moderator");
  g_assert_null(mod->description);

  for (gint a = GH_NIP29_AUTHZ_ALLOWED; a <= GH_NIP29_AUTHZ_UNKNOWN_POLICY; a++)
    g_assert_true(*gh_nip29_authz_to_string((GhNip29Authz)a) != '\0');
}

static void
test_member_snapshot_is_partial(void)
{
  g_autoptr(GhNip29Group) group = group_new(RELAY_A, &relay_a);
  g_auto(GStrv) none = NULL;
  g_assert_cmpint(gh_nip29_group_dup_members(group, &none), ==, GH_NIP29_MEMBERS_UNAVAILABLE);
  g_assert_null(none);
  g_assert_cmpint(gh_nip29_group_lookup_member(group, member.pk), ==, GH_NIP29_MEMBERSHIP_UNKNOWN);

  NostrEvent *members = event_new(NOSTR_KIND_SIMPLE_GROUP_MEMBERS, 100, GROUP_ID);
  event_tag(members, "p", owner.pk, NULL);
  event_tag(members, "p", member.pk, NULL);
  event_tag(members, "p", "not-a-pubkey", NULL);
  g_assert_cmpint(admit_free(group, sign(&relay_a, members)), ==, GH_NIP29_ADMISSION_ACCEPTED);

  g_auto(GStrv) listed = NULL;
  g_assert_cmpint(gh_nip29_group_dup_members(group, &listed), ==, GH_NIP29_MEMBERS_PARTIAL);
  g_assert_true(strv_equal((const gchar *const *)listed,
                           (const gchar *const[]){ owner.pk, member.pk, NULL }));
  g_assert_cmpint(gh_nip29_group_dup_members(group, NULL), ==, GH_NIP29_MEMBERS_PARTIAL);
  g_assert_cmpint(gh_nip29_group_lookup_member(group, member.pk), ==, GH_NIP29_MEMBERSHIP_LISTED);
  /* Missing from a possibly partial list is not proof of non-membership. */
  g_assert_cmpint(gh_nip29_group_lookup_member(group, stranger.pk), ==, GH_NIP29_MEMBERSHIP_UNKNOWN);

  NostrEvent *forged = event_new(NOSTR_KIND_SIMPLE_GROUP_MEMBERS, 200, GROUP_ID);
  event_tag(forged, "p", stranger.pk, NULL);
  g_assert_cmpint(admit_free(group, sign(&attacker, forged)), ==, GH_NIP29_ADMISSION_WRONG_AUTHOR);
  g_assert_cmpint(gh_nip29_group_lookup_member(group, stranger.pk), ==, GH_NIP29_MEMBERSHIP_UNKNOWN);

  /* A newer snapshot replaces the list; it does not merge with it. */
  NostrEvent *replacement = event_new(NOSTR_KIND_SIMPLE_GROUP_MEMBERS, 200, GROUP_ID);
  event_tag(replacement, "p", stranger.pk, NULL);
  g_assert_cmpint(admit_free(group, sign(&relay_a, replacement)), ==, GH_NIP29_ADMISSION_ACCEPTED);
  g_assert_cmpint(gh_nip29_group_lookup_member(group, stranger.pk), ==, GH_NIP29_MEMBERSHIP_LISTED);
  g_assert_cmpint(gh_nip29_group_lookup_member(group, member.pk), ==, GH_NIP29_MEMBERSHIP_UNKNOWN);
}

static NostrEvent *
full_metadata_event(gint64 created_at)
{
  NostrEvent *event = event_new(NOSTR_KIND_SIMPLE_GROUP_METADATA, created_at, GROUP_ID);
  event_tag(event, "name", "Pizza Lovers", NULL);
  event_tag(event, "picture", "https://pizza.example/pizza.png", NULL);
  event_tag(event, "banner", "https://pizza.example/banner.png", NULL);
  event_tag(event, "about", "a group for people who love pizza", NULL);
  event_tag(event, "private", NULL);
  event_tag(event, "restricted", NULL);
  event_tag(event, "hidden", NULL);
  event_tag(event, "closed", NULL);
  event_tag(event, "livekit", NULL);
  event_tag(event, "supported_kinds", "9", "11", "bogus", "9", NULL);
  event_tag(event, "parent", "food", NULL);
  event_tag(event, "child", "pizza-nyc", NULL);
  event_tag(event, "child", "pizza-rome", NULL);
  return sign(&relay_a, event);
}

static void
test_metadata_flags(void)
{
  g_autoptr(GhNip29Group) group = group_new(RELAY_A, &relay_a);
  g_assert_null(gh_nip29_group_dup_metadata(group));
  g_assert_cmpint(admit_free(group, full_metadata_event(100)), ==, GH_NIP29_ADMISSION_ACCEPTED);

  g_autoptr(GhNip29Metadata) md = gh_nip29_group_dup_metadata(group);
  g_autoptr(GhNip29Metadata) copy = gh_nip29_metadata_copy(md);
  GhNip29Metadata *views[] = { md, copy };
  for (gsize i = 0; i < G_N_ELEMENTS(views); i++) {
    GhNip29Metadata *m = views[i];
    g_assert_cmpstr(m->name, ==, "Pizza Lovers");
    g_assert_cmpstr(m->picture, ==, "https://pizza.example/pizza.png");
    g_assert_cmpstr(m->banner, ==, "https://pizza.example/banner.png");
    g_assert_cmpstr(m->about, ==, "a group for people who love pizza");
    g_assert_true(m->is_private && m->is_restricted && m->is_hidden && m->is_closed);
    g_assert_true(m->has_livekit);
    g_assert_true(m->has_supported_kinds);
    g_assert_cmpuint(m->n_supported_kinds, ==, 2);
    g_assert_cmpint(m->supported_kinds[0], ==, 9);
    g_assert_cmpint(m->supported_kinds[1], ==, 11);
    g_assert_true(gh_nip29_metadata_supports_kind(m, 11));
    g_assert_false(gh_nip29_metadata_supports_kind(m, 1));
    g_assert_cmpstr(m->parent, ==, "food");
    g_assert_true(strv_equal((const gchar *const *)m->children,
                             (const gchar *const[]){ "pizza-nyc", "pizza-rome", NULL }));
  }

  /* A newer snapshot is authoritative in full: absent flags are cleared and
   * an absent supported_kinds tag means every kind. */
  g_assert_cmpint(admit_free(group, metadata_event(&relay_a, 101, "Pizza")), ==,
                  GH_NIP29_ADMISSION_ACCEPTED);
  g_autoptr(GhNip29Metadata) plain = gh_nip29_group_dup_metadata(group);
  g_assert_false(plain->is_private || plain->is_restricted || plain->is_hidden ||
                 plain->is_closed || plain->has_livekit || plain->has_supported_kinds);
  g_assert_true(gh_nip29_metadata_supports_kind(plain, 1));
  g_assert_null(plain->parent);
  g_assert_true(strv_equal((const gchar *const *)plain->children, (const gchar *const[]){ NULL }));

  NostrEvent *av_only = event_new(NOSTR_KIND_SIMPLE_GROUP_METADATA, 102, GROUP_ID);
  event_tag(av_only, "supported_kinds", NULL);
  g_assert_cmpint(admit_free(group, sign(&relay_a, av_only)), ==, GH_NIP29_ADMISSION_ACCEPTED);
  g_autoptr(GhNip29Metadata) none = gh_nip29_group_dup_metadata(group);
  g_assert_true(none->has_supported_kinds);
  g_assert_cmpuint(none->n_supported_kinds, ==, 0);
  g_assert_false(gh_nip29_metadata_supports_kind(none, NOSTR_KIND_SIMPLE_GROUP_CHAT_MESSAGE));
}

/* ---- Templates ----------------------------------------------------------- */

#define AUTHOR "1111111111111111111111111111111111111111111111111111111111111111"
#define TARGET "2222222222222222222222222222222222222222222222222222222222222222"
#define OTHER "3333333333333333333333333333333333333333333333333333333333333333"
#define FILL "0123456789abcdef0123456789abcdef0123456789abcdef01234567"
#define EV1 "eb96c864" FILL
#define EV2 "2db75638" FILL
#define EV3 "b5d1065f" FILL
#define EV4 "0badc0de" FILL
#define EV_OWN "ffffffff" FILL
#define PREVIOUS "[\"previous\",\"eb96c864\",\"2db75638\",\"b5d1065f\"]"
#define HEAD(kind) "{\"pubkey\":\"" AUTHOR "\",\"created_at\":1760000000,\"kind\":" kind ",\"tags\":[[\"h\",\"pizza\"],"

static gchar *
fake_event_id(guint32 prefix)
{
  return g_strdup_printf("%08x" FILL, prefix);
}

static void
test_previous_selection(void)
{
  g_autoptr(GError) error = NULL;
  GhNip29TimelineRef refs[60];
  gchar *ids[60];
  for (guint i = 0; i < G_N_ELEMENTS(refs); i++) {
    ids[i] = fake_event_id(0xa0000000u + i);
    refs[i].event_id = ids[i];
    refs[i].pubkey = (i == 0 || i == 2) ? AUTHOR : OTHER;
  }

  /* Newest first, the author's own events skipped. */
  g_auto(GStrv) newest = gh_nip29_select_previous(AUTHOR, refs, G_N_ELEMENTS(refs), &error);
  g_assert_no_error(error);
  g_assert_true(strv_equal((const gchar *const *)newest,
                           (const gchar *const[]){ "a0000001", "a0000003", "a0000004", NULL }));

  /* Only the last 50 events seen count, own events included in that window. */
  for (guint i = 0; i < 49; i++)
    refs[i].pubkey = AUTHOR;
  g_auto(GStrv) window = gh_nip29_select_previous(AUTHOR, refs, G_N_ELEMENTS(refs), &error);
  g_assert_no_error(error);
  g_assert_true(strv_equal((const gchar *const *)window, (const gchar *const[]){ "a0000031", NULL }));
  refs[49].pubkey = AUTHOR;
  g_auto(GStrv) empty = gh_nip29_select_previous(AUTHOR, refs, G_N_ELEMENTS(refs), &error);
  g_assert_no_error(error);
  g_assert_null(empty[0]);

  /* Entries past the window are never read; malformed ones inside fail. */
  refs[55].event_id = "not-an-id";
  g_auto(GStrv) ignored = gh_nip29_select_previous(AUTHOR, refs, G_N_ELEMENTS(refs), &error);
  g_assert_no_error(error);
  refs[10].event_id = "not-an-id";
  g_assert_null(gh_nip29_select_previous(AUTHOR, refs, G_N_ELEMENTS(refs), &error));
  g_assert_error(error, GH_NIP29_ERROR, GH_NIP29_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);

  /* The same prefix is cited once. */
  const GhNip29TimelineRef repeated[] = { { EV1, OTHER }, { EV1, OTHER }, { EV2, OTHER } };
  g_auto(GStrv) unique = gh_nip29_select_previous(AUTHOR, repeated, G_N_ELEMENTS(repeated), &error);
  g_assert_no_error(error);
  g_assert_true(strv_equal((const gchar *const *)unique,
                           (const gchar *const[]){ "eb96c864", "2db75638", NULL }));

  g_auto(GStrv) nothing = gh_nip29_select_previous(AUTHOR, NULL, 0, &error);
  g_assert_no_error(error);
  g_assert_null(nothing[0]);
  g_assert_null(gh_nip29_select_previous("nope", repeated, 1, &error));
  g_assert_error(error, GH_NIP29_ERROR, GH_NIP29_ERROR_INVALID_PUBKEY);

  for (guint i = 0; i < G_N_ELEMENTS(ids); i++)
    g_free(ids[i]);
}

static void
assert_json(gchar *json, GError *error, const gchar *expected)
{
  g_assert_no_error(error);
  g_assert_cmpstr(json, ==, expected);
  g_free(json);
}

static void
test_templates_exact(void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip29GroupKey) key = gh_nip29_group_key_new(RELAY_A, GROUP_ID, &error);
  g_assert_no_error(error);
  const GhNip29TimelineRef recent[] = {
    { EV1, OTHER }, { EV_OWN, AUTHOR }, { EV2, OTHER }, { EV3, OTHER }, { EV4, OTHER },
  };
  const GhNip29TemplateContext ctx = { AUTHOR, 1760000000, recent, G_N_ELEMENTS(recent) };
  const GhNip29TemplateContext quiet = { AUTHOR, 1760000000, NULL, 0 };

  assert_json(gh_nip29_template_chat(key, &ctx, "Hello, \"pizza\" lovers\n", &error), error,
              HEAD("9") PREVIOUS "],\"content\":\"Hello, \\\"pizza\\\" lovers\\n\"}");
  assert_json(gh_nip29_template_join_request(key, &ctx, "let me in", "INVITE-1", &error), error,
              HEAD("9021") "[\"code\",\"INVITE-1\"]," PREVIOUS "],\"content\":\"let me in\"}");
  assert_json(gh_nip29_template_join_request(key, &ctx, NULL, "", &error), error,
              HEAD("9021") PREVIOUS "],\"content\":\"\"}");
  assert_json(gh_nip29_template_join_request(key, &quiet, NULL, NULL, &error), error,
              "{\"pubkey\":\"" AUTHOR "\",\"created_at\":1760000000,\"kind\":9021,"
              "\"tags\":[[\"h\",\"pizza\"]],\"content\":\"\"}");
  assert_json(gh_nip29_template_leave_request(key, &ctx, "bye", &error), error,
              HEAD("9022") PREVIOUS "],\"content\":\"bye\"}");
  const gchar *const roles[] = { "moderator", "gardener", "moderator", NULL };
  assert_json(gh_nip29_template_put_user(key, &ctx, TARGET, roles, NULL, &error), error,
              HEAD("9000") "[\"p\",\"" TARGET "\",\"moderator\",\"gardener\"]," PREVIOUS
              "],\"content\":\"\"}");
  assert_json(gh_nip29_template_put_user(key, &ctx, TARGET, NULL, "welcome", &error), error,
              HEAD("9000") "[\"p\",\"" TARGET "\"]," PREVIOUS "],\"content\":\"welcome\"}");
  assert_json(gh_nip29_template_remove_user(key, &ctx, TARGET, "spam", &error), error,
              HEAD("9001") "[\"p\",\"" TARGET "\"]," PREVIOUS "],\"content\":\"spam\"}");
  assert_json(gh_nip29_template_delete_event(key, &ctx, EV4, NULL, &error), error,
              HEAD("9005") "[\"e\",\"" EV4 "\"]," PREVIOUS "],\"content\":\"\"}");
  assert_json(gh_nip29_template_create_invite(key, &ctx, "INVITE-2", NULL, &error), error,
              HEAD("9009") "[\"code\",\"INVITE-2\"]," PREVIOUS "],\"content\":\"\"}");

  /* edit-metadata replaces the whole state: editing a copy of the admitted
   * snapshot keeps its supported kinds, parent and children. */
  g_autoptr(GhNip29Group) group = group_new(RELAY_A, &relay_a);
  g_assert_cmpint(admit_free(group, full_metadata_event(100)), ==, GH_NIP29_ADMISSION_ACCEPTED);
  g_autoptr(GhNip29Metadata) edit = gh_nip29_group_dup_metadata(group);
  g_free(edit->name);
  edit->name = g_strdup("Pizza Fans");
  edit->is_hidden = FALSE;
  assert_json(gh_nip29_template_edit_metadata(key, &ctx, edit, "rename", &error), error,
              HEAD("9002")
              "[\"name\",\"Pizza Fans\"],"
              "[\"picture\",\"https://pizza.example/pizza.png\"],"
              "[\"banner\",\"https://pizza.example/banner.png\"],"
              "[\"about\",\"a group for people who love pizza\"],"
              "[\"private\"],[\"restricted\"],[\"closed\"],[\"livekit\"],"
              "[\"supported_kinds\",\"9\",\"11\"],"
              "[\"parent\",\"food\"],[\"child\",\"pizza-nyc\"],[\"child\",\"pizza-rome\"],"
              PREVIOUS "],\"content\":\"rename\"}");
  g_autoptr(GhNip29Metadata) minimal = gh_nip29_metadata_new();
  minimal->name = g_strdup("Pizza");
  minimal->has_supported_kinds = TRUE; /* AV-only: an empty supported_kinds tag */
  assert_json(gh_nip29_template_edit_metadata(key, &quiet, minimal, NULL, &error), error,
              "{\"pubkey\":\"" AUTHOR "\",\"created_at\":1760000000,\"kind\":9002,"
              "\"tags\":[[\"h\",\"pizza\"],[\"name\",\"Pizza\"],[\"supported_kinds\"]],"
              "\"content\":\"\"}");
}

static void
test_template_validation(void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip29GroupKey) key = gh_nip29_group_key_new(RELAY_A, GROUP_ID, &error);
  const GhNip29TemplateContext ctx = { AUTHOR, 1760000000, NULL, 0 };
  const GhNip29TemplateContext no_time = { AUTHOR, 0, NULL, 0 };
  const GhNip29TemplateContext bad_author = { "abc", 1760000000, NULL, 0 };
  const GhNip29TemplateContext bad_refs = { AUTHOR, 1760000000, NULL, 2 };

#define EXPECT_ERROR(call, code)                                   \
  G_STMT_START {                                                   \
    g_assert_null(call);                                           \
    g_assert_error(error, GH_NIP29_ERROR, code);                   \
    g_clear_error(&error);                                         \
  } G_STMT_END

  EXPECT_ERROR(gh_nip29_template_chat(key, &ctx, "", &error), GH_NIP29_ERROR_INVALID_ARGUMENT);
  EXPECT_ERROR(gh_nip29_template_chat(key, &ctx, NULL, &error), GH_NIP29_ERROR_INVALID_ARGUMENT);
  EXPECT_ERROR(gh_nip29_template_chat(key, &ctx, "\xff", &error), GH_NIP29_ERROR_INVALID_ARGUMENT);
  EXPECT_ERROR(gh_nip29_template_chat(key, &no_time, "hi", &error), GH_NIP29_ERROR_INVALID_ARGUMENT);
  EXPECT_ERROR(gh_nip29_template_chat(key, &bad_author, "hi", &error), GH_NIP29_ERROR_INVALID_PUBKEY);
  EXPECT_ERROR(gh_nip29_template_chat(key, &bad_refs, "hi", &error), GH_NIP29_ERROR_INVALID_ARGUMENT);
  EXPECT_ERROR(gh_nip29_template_chat(NULL, &ctx, "hi", &error), GH_NIP29_ERROR_INVALID_ARGUMENT);
  EXPECT_ERROR(gh_nip29_template_put_user(key, &ctx, "abc", NULL, NULL, &error),
               GH_NIP29_ERROR_INVALID_PUBKEY);
  const gchar *const empty_role[] = { "", NULL };
  EXPECT_ERROR(gh_nip29_template_put_user(key, &ctx, TARGET, empty_role, NULL, &error),
               GH_NIP29_ERROR_INVALID_ARGUMENT);
  EXPECT_ERROR(gh_nip29_template_remove_user(key, &ctx, OTHER "0", NULL, &error),
               GH_NIP29_ERROR_INVALID_PUBKEY);
  EXPECT_ERROR(gh_nip29_template_delete_event(key, &ctx, "eb96c864", NULL, &error),
               GH_NIP29_ERROR_INVALID_ARGUMENT);
  EXPECT_ERROR(gh_nip29_template_create_invite(key, &ctx, "", NULL, &error),
               GH_NIP29_ERROR_INVALID_ARGUMENT);
  EXPECT_ERROR(gh_nip29_template_leave_request(key, &ctx, "\xc3", &error),
               GH_NIP29_ERROR_INVALID_ARGUMENT);
  EXPECT_ERROR(gh_nip29_template_edit_metadata(key, &ctx, NULL, NULL, &error),
               GH_NIP29_ERROR_INVALID_ARGUMENT);

  g_autoptr(GhNip29Metadata) md = gh_nip29_metadata_new();
  md->parent = g_strdup(GROUP_ID);
  EXPECT_ERROR(gh_nip29_template_edit_metadata(key, &ctx, md, NULL, &error),
               GH_NIP29_ERROR_INVALID_GROUP_ID);
  g_clear_pointer(&md->parent, g_free);
  md->children = g_strsplit("pizza-nyc,Bad Id", ",", -1);
  EXPECT_ERROR(gh_nip29_template_edit_metadata(key, &ctx, md, NULL, &error),
               GH_NIP29_ERROR_INVALID_GROUP_ID);
  g_clear_pointer(&md->children, g_strfreev);
  gint kinds[] = { 9, 70000 };
  md->has_supported_kinds = TRUE;
  md->supported_kinds = g_new(gint, G_N_ELEMENTS(kinds));
  memcpy(md->supported_kinds, kinds, sizeof(kinds));
  md->n_supported_kinds = G_N_ELEMENTS(kinds);
  EXPECT_ERROR(gh_nip29_template_edit_metadata(key, &ctx, md, NULL, &error),
               GH_NIP29_ERROR_INVALID_ARGUMENT);
#undef EXPECT_ERROR
}

static void
test_template_is_signable(void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip29GroupKey) key = gh_nip29_group_key_new(RELAY_A, GROUP_ID, &error);
  const GhNip29TimelineRef recent[] = { { EV1, member.pk } };
  const GhNip29TemplateContext ctx = { owner.pk, 1760000000, recent, 1 };
  g_autofree gchar *json = gh_nip29_template_chat(key, &ctx, "signed hello", &error);
  g_assert_no_error(error);

  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_unsigned(event, json, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  g_assert_null(event->sig);
  g_assert_cmpint(nostr_event_get_kind(event), ==, NOSTR_KIND_SIMPLE_GROUP_CHAT_MESSAGE);
  g_assert_cmpstr(nostr_event_get_pubkey(event), ==, owner.pk);
  sign(&owner, event);
  g_assert_cmpstr(nostr_event_get_pubkey(event), ==, owner.pk);
  const NostrTags *tags = nostr_event_get_tags(event);
  g_assert_cmpuint(nostr_tags_size(tags), ==, 2);
  g_assert_cmpstr(nostr_tag_get(nostr_tags_get(tags, 0), 1), ==, GROUP_ID);
  g_assert_cmpstr(nostr_tag_get(nostr_tags_get(tags, 1), 1), ==, "eb96c864");
  nostr_event_free(event);
}

/* ---- qp24.12.2 residuals ----------------------------------------------------- */

/* A kind:9002 built from the admitted metadata carries the relay's unmodelled
 * tags again (NIP-29 edits replace); d/h/previous and out-of-bounds tags are
 * never carried. */
static void
test_edit_keeps_unknown_tags(void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip29Group) group = group_new(RELAY_A, &relay_a);
  NostrEvent *event = event_new(NOSTR_KIND_SIMPLE_GROUP_METADATA, 100, GROUP_ID);
  event_tag(event, "name", "Pizza", NULL);
  event_tag(event, "t", "food", NULL);
  event_tag(event, "x-relay-policy", "slow", "30", NULL);
  event_tag(event, "previous", "eb96c864", NULL);
  event_tag(event, "h", GROUP_ID, NULL);
  g_autofree gchar *huge = g_strnfill(GH_NIP29_MAX_EXTRA_TAG_VALUE_BYTES + 1, 'x');
  event_tag(event, "x-huge", huge, NULL);
  event_tag(event, "public", NULL);
  g_assert_cmpint(admit_free(group, sign(&relay_a, event)), ==, GH_NIP29_ADMISSION_ACCEPTED);

  g_autoptr(GhNip29Metadata) metadata = gh_nip29_group_dup_metadata(group);
  g_assert_nonnull(metadata->extra_tags);
  g_assert_cmpuint(metadata->extra_tags->len, ==, 3);
  g_assert_true(strv_equal(g_ptr_array_index(metadata->extra_tags, 0),
                           (const gchar *const[]){ "t", "food", NULL }));
  g_assert_true(strv_equal(g_ptr_array_index(metadata->extra_tags, 1),
                           (const gchar *const[]){ "x-relay-policy", "slow", "30", NULL }));
  g_assert_true(strv_equal(g_ptr_array_index(metadata->extra_tags, 2),
                           (const gchar *const[]){ "public", NULL }));
  g_autoptr(GhNip29Metadata) copy = gh_nip29_metadata_copy(metadata);
  g_assert_cmpuint(copy->extra_tags->len, ==, 3);

  g_autoptr(GhNip29GroupKey) key = gh_nip29_group_key_new(RELAY_A, GROUP_ID, &error);
  const GhNip29TemplateContext quiet = { AUTHOR, 1760000000, NULL, 0 };
  g_free(copy->name);
  copy->name = g_strdup("Pizza Fans");
  assert_json(gh_nip29_template_edit_metadata(key, &quiet, copy, NULL, &error), error,
              "{\"pubkey\":\"" AUTHOR "\",\"created_at\":1760000000,\"kind\":9002,"
              "\"tags\":[[\"h\",\"pizza\"],[\"name\",\"Pizza Fans\"],[\"t\",\"food\"],"
              "[\"x-relay-policy\",\"slow\",\"30\"],[\"public\"]],\"content\":\"\"}");

  /* A caller cannot smuggle a modelled field (or h/d) in as an extra tag. */
  g_ptr_array_add(copy->extra_tags, g_strsplit("closed", " ", -1));
  g_assert_null(gh_nip29_template_edit_metadata(key, &quiet, copy, NULL, &error));
  g_assert_error(error, GH_NIP29_ERROR, GH_NIP29_ERROR_INVALID_ARGUMENT);
  g_clear_error(&error);
  g_ptr_array_remove_index(copy->extra_tags, copy->extra_tags->len - 1);
  g_ptr_array_add(copy->extra_tags, g_strsplit("h other", " ", -1));
  g_assert_null(gh_nip29_template_edit_metadata(key, &quiet, copy, NULL, &error));
  g_assert_error(error, GH_NIP29_ERROR, GH_NIP29_ERROR_INVALID_ARGUMENT);
}

/* A snapshot dated beyond the skew bound is refused, so it cannot pin its
 * kind; one within the bound is admitted as before. */
static void
test_future_snapshot_bounded(void)
{
  const gint64 now = 1760000000;
  g_autoptr(GhNip29Group) group = group_new(RELAY_A, &relay_a);
  NostrEvent *future = metadata_event(&relay_a, now + GH_NIP29_MAX_FUTURE_SKEW_SECONDS + 1,
                                      "From the future");
  g_assert_cmpint(gh_nip29_group_admit_at(group, future, now), ==, GH_NIP29_ADMISSION_FUTURE);
  g_assert_null(gh_nip29_group_get_snapshot_id(group, NOSTR_KIND_SIMPLE_GROUP_METADATA, NULL));
  /* A correct later snapshot is still admitted afterwards. */
  NostrEvent *current = metadata_event(&relay_a, now - 10, "Pizza");
  g_assert_cmpint(gh_nip29_group_admit_at(group, current, now), ==, GH_NIP29_ADMISSION_ACCEPTED);
  nostr_event_free(current);
  NostrEvent *skewed = metadata_event(&relay_a, now + GH_NIP29_MAX_FUTURE_SKEW_SECONDS,
                                      "Slightly ahead");
  g_assert_cmpint(gh_nip29_group_admit_at(group, skewed, now), ==, GH_NIP29_ADMISSION_ACCEPTED);
  nostr_event_free(skewed);
  /* Once the clock has caught up, the same event is admissible. */
  g_assert_cmpint(gh_nip29_group_admit_at(group, future, now + 2), ==,
                  GH_NIP29_ADMISSION_ACCEPTED);
  nostr_event_free(future);
  g_assert_cmpstr(gh_nip29_admission_to_string(GH_NIP29_ADMISSION_FUTURE), ==,
                  "dated too far in the future");
}

/* The timeline ring keeps the newest window by (created_at, id), each id
 * once, and feeds the template context newest first. */
static void
test_timeline_ring(void)
{
  g_autoptr(GhNip29Timeline) timeline = gh_nip29_timeline_new();
  g_autofree gchar *first = fake_event_id(0x10000000u);
  g_assert_true(gh_nip29_timeline_add(timeline, first, OTHER, 5));
  g_assert_false(gh_nip29_timeline_add(timeline, first, OTHER, 5));
  g_assert_false(gh_nip29_timeline_add(timeline, "nope", OTHER, 5));
  g_assert_false(gh_nip29_timeline_add(timeline, first, "nope", 5));
  /* Backfill arrives in any order; the ring is kept newest first. */
  for (guint i = 0; i < 70; i++) {
    g_autofree gchar *id = fake_event_id(0xa0000000u + i);
    gh_nip29_timeline_add(timeline, id, i % 2 ? OTHER : AUTHOR, 1000 + (gint64)((i * 37) % 70));
  }
  g_assert_cmpuint(gh_nip29_timeline_get_length(timeline), ==, GH_NIP29_PREVIOUS_WINDOW);
  for (guint i = 1; i < GH_NIP29_PREVIOUS_WINDOW; i++) {
    const GhNip29TimelineEntry *newer = gh_nip29_timeline_get_entry(timeline, i - 1);
    const GhNip29TimelineEntry *older = gh_nip29_timeline_get_entry(timeline, i);
    g_assert_cmpint(newer->created_at, >=, older->created_at);
  }
  g_assert_cmpint(gh_nip29_timeline_get_entry(timeline, 0)->created_at, ==, 1069);
  /* The oldest (created_at 5) fell out of the window; older ones stay out. */
  g_assert_false(gh_nip29_timeline_add(timeline, first, OTHER, 5));
  gsize n = 0;
  const GhNip29TimelineRef *refs = gh_nip29_timeline_get_refs(timeline, &n);
  g_assert_cmpuint(n, ==, GH_NIP29_PREVIOUS_WINDOW);
  g_autoptr(GError) error = NULL;
  g_auto(GStrv) previous = gh_nip29_select_previous(AUTHOR, refs, n, &error);
  g_assert_no_error(error);
  g_assert_cmpuint(g_strv_length(previous), ==, GH_NIP29_PREVIOUS_REFS);
  for (guint i = 0; previous[i]; i++)
    g_assert_true(g_str_has_prefix(previous[i], "a"));
  /* A deleted event leaves the ring and the refs follow. */
  g_autofree gchar *newest = g_strdup(gh_nip29_timeline_get_entry(timeline, 0)->event_id);
  g_assert_true(gh_nip29_timeline_remove(timeline, newest));
  g_assert_false(gh_nip29_timeline_remove(timeline, newest));
  refs = gh_nip29_timeline_get_refs(timeline, &n);
  g_assert_cmpuint(n, ==, GH_NIP29_PREVIOUS_WINDOW - 1);
  for (gsize i = 0; i < n; i++)
    g_assert_cmpstr(refs[i].event_id, !=, newest);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  TestKey *keys[] = { &relay_a, &relay_b, &attacker, &owner, &moderator, &gardener, &member, &stranger };
  for (gsize i = 0; i < G_N_ELEMENTS(keys); i++)
    key_init(keys[i]);

  g_test_add_func("/groundhog/nip29/relay-url", test_relay_url_normalization);
  g_test_add_func("/groundhog/nip29/group-key", test_group_key_identity);
  g_test_add_func("/groundhog/nip29/relay-isolation", test_same_id_on_two_relays_is_isolated);
  g_test_add_func("/groundhog/nip29/admission-rejections", test_rejects_unverified_snapshots);
  g_test_add_func("/groundhog/nip29/newest-wins-tie-break", test_newest_wins_and_tie_break);
  g_test_add_func("/groundhog/nip29/authorization", test_admin_authorization);
  g_test_add_func("/groundhog/nip29/members-partial", test_member_snapshot_is_partial);
  g_test_add_func("/groundhog/nip29/metadata-flags", test_metadata_flags);
  g_test_add_func("/groundhog/nip29/previous", test_previous_selection);
  g_test_add_func("/groundhog/nip29/templates", test_templates_exact);
  g_test_add_func("/groundhog/nip29/template-validation", test_template_validation);
  g_test_add_func("/groundhog/nip29/template-signable", test_template_is_signable);
  g_test_add_func("/groundhog/nip29/edit-keeps-unknown-tags", test_edit_keeps_unknown_tags);
  g_test_add_func("/groundhog/nip29/future-snapshot-bounded", test_future_snapshot_bounded);
  g_test_add_func("/groundhog/nip29/timeline-ring", test_timeline_ring);
  gint rc = g_test_run();

  for (gsize i = 0; i < G_N_ELEMENTS(keys); i++)
    key_clear(keys[i]);
  return rc;
}
