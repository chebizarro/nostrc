#include "../gn-communikeys-community-service.h"
#include "../model/gn-communikeys-message-item.h"
#include "../model/gn-communikeys-section-item.h"
#include "../model/gn-communikeys-targeted-item.h"

#include <keys.h>
#include <nostr-event.h>
#include <nostr-tag.h>
#include <stdlib.h>

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NostrEvent, nostr_event_free)

typedef struct {
  char *community_sk;
  char *community_pk;
  char *member_sk;
  char *member_pk;
  char *outsider_sk;
  char *outsider_pk;
} Fixture;

static void add_tag(NostrEvent *event, NostrTag *tag) {
  nostr_tags_append((NostrTags *)nostr_event_get_tags(event), tag);
}
static gchar *sign_json(NostrEvent *event, const char *sk) {
  g_assert_cmpint(nostr_event_sign(event, sk), ==, 0);
  char *serialized = nostr_event_serialize_compact(event);
  g_assert_nonnull(serialized);
  gchar *json = g_strdup(serialized);
  free(serialized);
  return json;
}
static NostrEvent *new_event(int kind, const char *pubkey,
                             gint64 created_at, const char *content) {
  NostrEvent *event = nostr_event_new();
  g_assert_nonnull(event);
  nostr_event_set_kind(event, kind);
  nostr_event_set_pubkey(event, pubkey);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, content ? content : "");
  return event;
}
static gchar *definition_json(Fixture *f, const char *acl_publisher,
                              gint64 created_at) {
  g_autoptr(NostrEvent) event = new_event(
    CAS_COMMUNITY_DEFINITION, f->community_pk, created_at, "");
  add_tag(event, nostr_tag_new("r", "wss://community.example", NULL));
  add_tag(event, nostr_tag_new("content", "General", NULL));
  add_tag(event, nostr_tag_new("k", "30023", NULL));
  g_autofree gchar *general = g_strdup_printf(
    "30000:%s:General", acl_publisher);
  add_tag(event, nostr_tag_new("a", general,
                               "wss://community.example", NULL));
  add_tag(event, nostr_tag_new("content", "Chat", NULL));
  add_tag(event, nostr_tag_new("k", "9", NULL));
  g_autofree gchar *chat = g_strdup_printf(
    "30000:%s:Chat", acl_publisher);
  add_tag(event, nostr_tag_new("a", chat,
                               "wss://community.example", NULL));
  add_tag(event, nostr_tag_new("badge",
    "30009:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa:member",
    NULL));
  add_tag(event, nostr_tag_new("content", "Threads", NULL));
  add_tag(event, nostr_tag_new("k", "11", "threads", NULL));
  g_autofree gchar *threads = g_strdup_printf(
    "30000:%s:Threads", acl_publisher);
  add_tag(event, nostr_tag_new("a", threads,
                               "wss://community.example", NULL));
  return sign_json(event, f->community_sk);
}
static gchar *acl_json(Fixture *f, const char *identifier,
                       const char *member, gint64 created_at) {
  g_autoptr(NostrEvent) event = new_event(
    NOSTR_COMMUNIKEYS_KIND_PROFILE_LIST,
    f->community_pk, created_at, "");
  add_tag(event, nostr_tag_new("d", identifier, NULL));
  if (member) {
    add_tag(event, nostr_tag_new("p", member, NULL));
    add_tag(event, nostr_tag_new("p", member, "duplicate", NULL));
  }
  return sign_json(event, f->community_sk);
}
static gchar *exclusive_json(const char *sk, const char *pubkey,
                             const char *community, int kind,
                             const char *content, gint64 created_at) {
  g_autoptr(NostrEvent) event =
    new_event(kind, pubkey, created_at, content);
  g_assert_true(nostr_communikeys_exclusive_add_h(event, community));
  return sign_json(event, sk);
}
static gchar *original_json(Fixture *f, gchar **id_out) {
  g_autoptr(NostrEvent) event =
    new_event(30023, f->member_pk, 40, "A verified long-form post");
  gchar *json = sign_json(event, f->member_sk);
  *id_out = nostr_event_get_id(event);
  return json;
}
static gchar *target_json(Fixture *f, const char *reference,
                          const char *author_sk, const char *author_pk) {
  nostr_communikeys_target_t target = {
    .pubkey = f->community_pk,
    .relay = "wss://community.example"
  };
  nostr_communikeys_targeted_publication_t publication = {
    .identifier = "target-1",
    .reference_type = NOSTR_COMMUNIKEYS_REFERENCE_EVENT,
    .reference = (char *)reference,
    .reference_relay = "wss://source.example",
    .reference_author = (char *)author_pk,
    .original_kind = 30023,
    .original_author = (char *)author_pk,
    .targets = &target,
    .targets_len = 1
  };
  g_autoptr(NostrEvent) event =
    nostr_communikeys_targeted_publication_to_event(&publication, 50);
  g_assert_nonnull(event);
  return sign_json(event, author_sk);
}

static void fixture_setup(Fixture *f, gconstpointer data) {
  (void)data;
  f->community_sk = nostr_key_generate_private();
  f->community_pk = nostr_key_get_public(f->community_sk);
  f->member_sk = nostr_key_generate_private();
  f->member_pk = nostr_key_get_public(f->member_sk);
  f->outsider_sk = nostr_key_generate_private();
  f->outsider_pk = nostr_key_get_public(f->outsider_sk);
  g_assert_nonnull(f->community_pk);
  g_assert_nonnull(f->member_pk);
  g_assert_nonnull(f->outsider_pk);
}
static void fixture_teardown(Fixture *f, gconstpointer data) {
  (void)data;
  free(f->community_sk);
  free(f->community_pk);
  free(f->member_sk);
  free(f->member_pk);
  free(f->outsider_sk);
  free(f->outsider_pk);
}

static void test_sections_acl_chat_and_revocation(
    Fixture *f, gconstpointer data) {
  (void)data;
  g_autoptr(GnCommunikeysCommunityService) service =
    gn_communikeys_community_service_new_offline(f->member_pk);
  g_autofree gchar *definition =
    definition_json(f, f->community_pk, 10);
  g_assert_true(gn_communikeys_community_service_ingest_event(
    service, definition));
  g_assert_cmpuint(g_list_model_get_n_items(
    gn_communikeys_community_service_get_model(service)), ==, 1);

  g_autoptr(GnCommunikeysCommunityItem) community =
    gn_communikeys_community_service_lookup_community(
      service, f->community_pk);
  g_assert_nonnull(community);
  g_assert_cmpuint(
    gn_communikeys_community_item_get_section_count(community), ==, 3);
  g_autoptr(GnCommunikeysSectionItem) chat =
    gn_communikeys_community_item_find_section(community, "Chat");
  g_assert_cmpint(gn_communikeys_section_item_get_acl_state(chat),
                  ==, GN_COMMUNIKEYS_ACL_UNRESOLVED);
  g_assert_cmpuint(gn_communikeys_section_item_get_badge_count(chat), ==, 1);

  g_autofree gchar *acl = acl_json(f, "Chat", f->member_pk, 20);
  g_assert_true(gn_communikeys_community_service_ingest_event(service, acl));
  g_assert_cmpint(gn_communikeys_section_item_get_acl_state(chat),
                  ==, GN_COMMUNIKEYS_ACL_VERIFIED);
  g_assert_cmpuint(gn_communikeys_section_item_get_member_count(chat), ==, 1);
  g_assert_true(gn_communikeys_community_service_author_can_publish(
    service, f->community_pk, 9, f->member_pk));

  g_autofree gchar *message = exclusive_json(
    f->member_sk, f->member_pk, f->community_pk, 9, "hello", 30);
  g_assert_true(gn_communikeys_community_service_ingest_event(
    service, message));
  GListModel *messages = gn_communikeys_community_service_get_messages(
    service, f->community_pk);
  g_assert_cmpuint(g_list_model_get_n_items(messages), ==, 1);

  g_autofree gchar *spoof = exclusive_json(
    f->outsider_sk, f->outsider_pk, f->community_pk, 9, "spoof", 31);
  g_assert_false(gn_communikeys_community_service_ingest_event(
    service, spoof));
  g_assert_cmpuint(g_list_model_get_n_items(messages), ==, 1);

  g_autofree gchar *revoked = acl_json(f, "Chat", NULL, 32);
  g_assert_true(gn_communikeys_community_service_ingest_event(
    service, revoked));
  g_assert_false(gn_communikeys_community_service_author_can_publish(
    service, f->community_pk, 9, f->member_pk));
  g_assert_cmpuint(g_list_model_get_n_items(messages), ==, 0);
}

static void test_targeted_publication_requires_original_and_acl(
    Fixture *f, gconstpointer data) {
  (void)data;
  g_autoptr(GnCommunikeysCommunityService) service =
    gn_communikeys_community_service_new_offline(f->member_pk);
  g_autofree gchar *definition =
    definition_json(f, f->community_pk, 10);
  g_assert_true(gn_communikeys_community_service_ingest_event(
    service, definition));
  g_autofree gchar *general =
    acl_json(f, "General", f->member_pk, 20);
  g_assert_true(gn_communikeys_community_service_ingest_event(
    service, general));

  g_autofree gchar *original_id = NULL;
  g_autofree gchar *original = original_json(f, &original_id);
  g_assert_true(gn_communikeys_community_service_ingest_event(
    service, original));

  g_autofree gchar *target = target_json(
    f, original_id, f->member_sk, f->member_pk);
  g_assert_true(gn_communikeys_community_service_ingest_event(
    service, target));
  GListModel *targets = gn_communikeys_community_service_get_targets(
    service, f->community_pk);
  g_assert_cmpuint(g_list_model_get_n_items(targets), ==, 1);
  g_autoptr(GnCommunikeysTargetedItem) item =
    g_list_model_get_item(targets, 0);
  g_assert_cmpstr(
    gn_communikeys_targeted_item_get_original_content(item),
    ==, "A verified long-form post");

  g_autofree gchar *spoof = target_json(
    f, original_id, f->outsider_sk, f->outsider_pk);
  g_assert_false(gn_communikeys_community_service_ingest_event(
    service, spoof));
  g_assert_cmpuint(g_list_model_get_n_items(targets), ==, 1);
}

static void test_delegated_acl_is_untrusted(
    Fixture *f, gconstpointer data) {
  (void)data;
  g_autoptr(GnCommunikeysCommunityService) service =
    gn_communikeys_community_service_new_offline(f->member_pk);
  g_autofree gchar *definition =
    definition_json(f, f->outsider_pk, 10);
  g_assert_true(gn_communikeys_community_service_ingest_event(
    service, definition));
  g_autoptr(GnCommunikeysCommunityItem) community =
    gn_communikeys_community_service_lookup_community(
      service, f->community_pk);
  g_autoptr(GnCommunikeysSectionItem) chat =
    gn_communikeys_community_item_find_section(community, "Chat");
  g_assert_cmpint(gn_communikeys_section_item_get_acl_state(chat),
                  ==, GN_COMMUNIKEYS_ACL_UNTRUSTED_PUBLISHER);
  g_assert_false(gn_communikeys_community_service_author_can_publish(
    service, f->community_pk, 9, f->member_pk));
}

static void test_unsigned_definition_is_rejected(
    Fixture *f, gconstpointer data) {
  (void)data;
  g_autoptr(GnCommunikeysCommunityService) service =
    gn_communikeys_community_service_new_offline(f->member_pk);
  g_autoptr(NostrEvent) event = new_event(
    CAS_COMMUNITY_DEFINITION, f->community_pk, 10, "");
  add_tag(event, nostr_tag_new("r", "wss://community.example", NULL));
  char *serialized = nostr_event_serialize_compact(event);
  g_assert_nonnull(serialized);
  g_assert_false(gn_communikeys_community_service_ingest_event(
    service, serialized));
  free(serialized);
  g_assert_cmpuint(g_list_model_get_n_items(
    gn_communikeys_community_service_get_model(service)), ==, 0);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add("/communikeys/service/sections-acl-chat-revocation",
             Fixture, NULL, fixture_setup,
             test_sections_acl_chat_and_revocation, fixture_teardown);
  g_test_add("/communikeys/service/target-original-acl",
             Fixture, NULL, fixture_setup,
             test_targeted_publication_requires_original_and_acl,
             fixture_teardown);
  g_test_add("/communikeys/service/delegated-acl-untrusted",
             Fixture, NULL, fixture_setup,
             test_delegated_acl_is_untrusted, fixture_teardown);
  g_test_add("/communikeys/service/unsigned-rejected",
             Fixture, NULL, fixture_setup,
             test_unsigned_definition_is_rejected, fixture_teardown);
  return g_test_run();
}
