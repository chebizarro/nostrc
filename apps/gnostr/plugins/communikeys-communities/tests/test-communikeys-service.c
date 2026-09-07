#include "../gn-communikeys-community-service.h"
#include "../model/gn-communikeys-message-item.h"
#include "../model/gn-communikeys-section-item.h"
#include "../model/gn-communikeys-targeted-item.h"

#include <keys.h>
#include <nostr-event.h>
#include <nostr-tag.h>
#include <stdlib.h>
#include <string.h>

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NostrEvent, nostr_event_free)

/*
 * Communikeys V2 fixtures. The community ID is an OPAQUE identifier minted
 * from a discarded keypair (mirroring §Community ID creation); the owner,
 * the delegated list author, members, and the curator are all distinct real
 * signers.
 */
typedef struct {
  char *owner_sk;
  char *owner_pk;
  char *owner2_sk;
  char *owner2_pk;
  char *list_sk;
  char *list_pk;
  char *member_sk;
  char *member_pk;
  char *curator_sk;
  char *curator_pk;
  char *community_id; /* opaque; its private key was discarded */
  gchar *address;     /* 32222:<owner_pk>:<community_id> */
  gchar *address2;    /* 32222:<owner2_pk>:<community_id> (same-ID branch) */
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

/* V2 definition: kind 32222, d = community ID, required name, wss relays,
 * section-scoped profile-list identifiers authored by a DELEGATED signer.
 * The General section is sharded across two coordinates. */
static gchar *definition_json_for(Fixture *f, const char *owner_sk,
                                  const char *owner_pk, const char *list_pk,
                                  const char *name, gint64 created_at) {
  g_autoptr(NostrEvent) event = new_event(
    CAS_COMMUNITY_DEFINITION, owner_pk, created_at, "");
  add_tag(event, nostr_tag_new("d", f->community_id, NULL));
  add_tag(event, nostr_tag_new("name", name, NULL));
  add_tag(event, nostr_tag_new("r", "wss://community.example", NULL));
  add_tag(event, nostr_tag_new("blossom", "https://media.example", NULL));
  add_tag(event, nostr_tag_new("content", "General", NULL));
  add_tag(event, nostr_tag_new("k", "30023", NULL));
  g_autofree gchar *general = g_strdup_printf(
    "30000:%s:%s-general", list_pk, f->community_id);
  add_tag(event, nostr_tag_new("a", general,
                               "wss://community.example", NULL));
  g_autofree gchar *general2 = g_strdup_printf(
    "30000:%s:%s-general.2", list_pk, f->community_id);
  add_tag(event, nostr_tag_new("a", general2, NULL));
  add_tag(event, nostr_tag_new("content", "Chat", NULL));
  add_tag(event, nostr_tag_new("k", "9", NULL));
  g_autofree gchar *chat = g_strdup_printf(
    "30000:%s:%s-chat", list_pk, f->community_id);
  add_tag(event, nostr_tag_new("a", chat,
                               "wss://community.example", NULL));
  add_tag(event, nostr_tag_new("badge",
    "30009:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa:member",
    NULL));
  add_tag(event, nostr_tag_new("content", "Threads", NULL));
  add_tag(event, nostr_tag_new("k", "11", "threads", NULL));
  g_autofree gchar *threads = g_strdup_printf(
    "30000:%s:%s-threads", list_pk, f->community_id);
  add_tag(event, nostr_tag_new("a", threads,
                               "wss://community.example", NULL));
  return sign_json(event, owner_sk);
}

static gchar *definition_json(Fixture *f, gint64 created_at) {
  return definition_json_for(f, f->owner_sk, f->owner_pk, f->list_pk,
                             "Cascadia Builders", created_at);
}

/* Section profile lists are signed by the DELEGATED list author — under V2
 * this is the normal shape, not a trust violation. */
static gchar *acl_json(Fixture *f, const char *purpose_suffix,
                       const char *member, gint64 created_at) {
  g_autoptr(NostrEvent) event = new_event(
    NOSTR_COMMUNIKEYS_KIND_PROFILE_LIST,
    f->list_pk, created_at, "");
  g_autofree gchar *d = g_strdup_printf("%s-%s", f->community_id,
                                        purpose_suffix);
  add_tag(event, nostr_tag_new("d", d, NULL));
  if (member) {
    add_tag(event, nostr_tag_new("p", member, NULL));
    add_tag(event, nostr_tag_new("p", member, "duplicate hint ignored", NULL));
  }
  return sign_json(event, f->list_sk);
}

static gchar *exclusive_json(Fixture *f, const char *sk, const char *pubkey,
                             int kind, const char *content,
                             gint64 created_at) {
  g_autoptr(NostrEvent) event =
    new_event(kind, pubkey, created_at, content);
  g_assert_true(nostr_communikeys_exclusive_add_h(event, f->community_id));
  return sign_json(event, sk);
}

static gchar *original_json(Fixture *f, const char *sk, const char *pubkey,
                            gchar **id_out) {
  (void)f;
  g_autoptr(NostrEvent) event =
    new_event(30023, pubkey, 40, "A verified long-form post");
  gchar *json = sign_json(event, sk);
  *id_out = nostr_event_get_id(event);
  return json;
}

/* A V2 wrapper: e-source with author hint, one adjacent h+a pair naming the
 * exact branch. The signer is the curator and MAY differ from the original
 * author. */
static gchar *target_json(Fixture *f, const char *reference,
                          const char *original_author,
                          const char *curator_sk, const char *curator_pk) {
  nostr_communikeys_community_target_t target;
  memset(&target, 0, sizeof(target));
  memcpy(target.community_id, f->community_id, 64);
  g_assert_true(nostr_communikeys_branch_parse(f->address, &target.branch));
  target.relay = (char *)"wss://community.example";
  nostr_communikeys_targeted_publication_t publication = {
    .identifier = (char *)"target-1",
    .has_source = true,
    .reference_type = NOSTR_COMMUNIKEYS_REFERENCE_EVENT,
    .reference = (char *)reference,
    .reference_relay = (char *)"wss://source.example",
    .reference_author = (char *)original_author,
    .original_kind = 30023,
    .curator = (char *)curator_pk,
    .targets = &target,
    .targets_len = 1
  };
  g_autoptr(NostrEvent) event =
    nostr_communikeys_targeted_publication_to_event(&publication, 50);
  g_assert_nonnull(event);
  return sign_json(event, curator_sk);
}

static void fixture_setup(Fixture *f, gconstpointer data) {
  (void)data;
  f->owner_sk = nostr_key_generate_private();
  f->owner_pk = nostr_key_get_public(f->owner_sk);
  f->owner2_sk = nostr_key_generate_private();
  f->owner2_pk = nostr_key_get_public(f->owner2_sk);
  f->list_sk = nostr_key_generate_private();
  f->list_pk = nostr_key_get_public(f->list_sk);
  f->member_sk = nostr_key_generate_private();
  f->member_pk = nostr_key_get_public(f->member_sk);
  f->curator_sk = nostr_key_generate_private();
  f->curator_pk = nostr_key_get_public(f->curator_sk);
  /* Mint the opaque community ID and discard its private key. */
  char *discarded_sk = nostr_key_generate_private();
  f->community_id = nostr_key_get_public(discarded_sk);
  free(discarded_sk);
  g_assert_nonnull(f->owner_pk);
  g_assert_nonnull(f->owner2_pk);
  g_assert_nonnull(f->list_pk);
  g_assert_nonnull(f->member_pk);
  g_assert_nonnull(f->curator_pk);
  g_assert_nonnull(f->community_id);
  f->address = g_strdup_printf("32222:%s:%s", f->owner_pk, f->community_id);
  f->address2 = g_strdup_printf("32222:%s:%s", f->owner2_pk, f->community_id);
}
static void fixture_teardown(Fixture *f, gconstpointer data) {
  (void)data;
  free(f->owner_sk);
  free(f->owner_pk);
  free(f->owner2_sk);
  free(f->owner2_pk);
  free(f->list_sk);
  free(f->list_pk);
  free(f->member_sk);
  free(f->member_pk);
  free(f->curator_sk);
  free(f->curator_pk);
  free(f->community_id);
  g_free(f->address);
  g_free(f->address2);
}

static void test_sections_acl_chat_and_revocation(
    Fixture *f, gconstpointer data) {
  (void)data;
  g_autoptr(GnCommunikeysCommunityService) service =
    gn_communikeys_community_service_new_offline(f->member_pk);
  g_autofree gchar *definition = definition_json(f, 10);
  g_assert_true(gn_communikeys_community_service_ingest_event(
    service, definition));
  g_assert_cmpuint(g_list_model_get_n_items(
    gn_communikeys_community_service_get_model(service)), ==, 1);

  g_autoptr(GnCommunikeysCommunityItem) community =
    gn_communikeys_community_service_lookup_community(service, f->address);
  g_assert_nonnull(community);
  g_assert_cmpstr(gn_communikeys_community_item_get_address(community),
                  ==, f->address);
  g_assert_cmpstr(gn_communikeys_community_item_get_owner_pubkey(community),
                  ==, f->owner_pk);
  g_assert_cmpstr(gn_communikeys_community_item_get_community_id(community),
                  ==, f->community_id);
  g_assert_cmpstr(gn_communikeys_community_item_get_name(community),
                  ==, "Cascadia Builders");
  g_assert_cmpuint(
    gn_communikeys_community_item_get_section_count(community), ==, 3);
  g_autoptr(GnCommunikeysSectionItem) chat =
    gn_communikeys_community_item_find_section(community, "Chat");
  g_assert_cmpint(gn_communikeys_section_item_get_acl_state(chat),
                  ==, GN_COMMUNIKEYS_ACL_UNRESOLVED);
  g_assert_cmpuint(gn_communikeys_section_item_get_badge_count(chat), ==, 1);
  g_assert_cmpuint(
    gn_communikeys_section_item_get_profile_list_count(chat), ==, 1);
  g_assert_cmpstr(
    gn_communikeys_section_item_get_profile_list_author(chat, 0),
    ==, f->list_pk);

  /* The delegated list author's event is authoritative — no community-key
   * requirement under V2. */
  g_autofree gchar *acl = acl_json(f, "chat", f->member_pk, 20);
  g_assert_true(gn_communikeys_community_service_ingest_event(service, acl));
  g_assert_cmpint(gn_communikeys_section_item_get_acl_state(chat),
                  ==, GN_COMMUNIKEYS_ACL_VERIFIED);
  g_assert_cmpuint(gn_communikeys_section_item_get_member_count(chat), ==, 1);
  g_assert_true(gn_communikeys_community_service_author_can_publish(
    service, f->address, 9, f->member_pk));
  /* The owner retains inherent authority; the list author holds the
   * structural community-wide role. */
  g_assert_true(gn_communikeys_community_service_author_can_publish(
    service, f->address, 9, f->owner_pk));
  g_assert_true(gn_communikeys_community_service_author_can_publish(
    service, f->address, 9, f->list_pk));
  /* The community ID itself grants nothing. */
  g_assert_false(gn_communikeys_community_service_author_can_publish(
    service, f->address, 9, f->community_id));

  g_autofree gchar *message = exclusive_json(
    f, f->member_sk, f->member_pk, 9, "hello", 30);
  g_assert_true(gn_communikeys_community_service_ingest_event(
    service, message));
  GListModel *messages = gn_communikeys_community_service_get_messages(
    service, f->address);
  g_assert_cmpuint(g_list_model_get_n_items(messages), ==, 1);

  g_autofree gchar *spoof = exclusive_json(
    f, f->curator_sk, f->curator_pk, 9, "spoof", 31);
  g_assert_false(gn_communikeys_community_service_ingest_event(
    service, spoof));
  g_assert_cmpuint(g_list_model_get_n_items(messages), ==, 1);

  g_autofree gchar *revoked = acl_json(f, "chat", NULL, 32);
  g_assert_true(gn_communikeys_community_service_ingest_event(
    service, revoked));
  g_assert_false(gn_communikeys_community_service_author_can_publish(
    service, f->address, 9, f->member_pk));
  g_assert_cmpuint(g_list_model_get_n_items(messages), ==, 0);
}

static void test_multi_shard_union(Fixture *f, gconstpointer data) {
  (void)data;
  g_autoptr(GnCommunikeysCommunityService) service =
    gn_communikeys_community_service_new_offline(f->member_pk);
  g_autofree gchar *definition = definition_json(f, 10);
  g_assert_true(gn_communikeys_community_service_ingest_event(
    service, definition));

  /* The member appears ONLY in shard .2 of the General section; the grant
   * comes from the union across both referenced coordinates. */
  g_autofree gchar *shard1 = acl_json(f, "general", f->curator_pk, 20);
  g_autofree gchar *shard2 = acl_json(f, "general.2", f->member_pk, 20);
  g_assert_true(gn_communikeys_community_service_ingest_event(
    service, shard1));
  g_assert_false(gn_communikeys_community_service_author_can_publish(
    service, f->address, 30023, f->member_pk));
  g_assert_true(gn_communikeys_community_service_ingest_event(
    service, shard2));
  g_assert_true(gn_communikeys_community_service_author_can_publish(
    service, f->address, 30023, f->member_pk));
  g_assert_true(gn_communikeys_community_service_author_can_publish(
    service, f->address, 30023, f->curator_pk));

  /* The section view exposes the union across shards. */
  g_autoptr(GnCommunikeysCommunityItem) community =
    gn_communikeys_community_service_lookup_community(service, f->address);
  g_autoptr(GnCommunikeysSectionItem) general =
    gn_communikeys_community_item_find_section(community, "General");
  g_assert_cmpuint(
    gn_communikeys_section_item_get_profile_list_count(general), ==, 2);
  g_assert_cmpuint(gn_communikeys_section_item_get_member_count(general), ==, 2);
  g_assert_true(gn_communikeys_section_item_has_member(general, f->member_pk));
  g_assert_true(gn_communikeys_section_item_has_member(general, f->curator_pk));
}

static void test_curator_targeted_publication(Fixture *f,
                                              gconstpointer data) {
  (void)data;
  g_autoptr(GnCommunikeysCommunityService) service =
    gn_communikeys_community_service_new_offline(f->member_pk);
  g_autofree gchar *definition = definition_json(f, 10);
  g_assert_true(gn_communikeys_community_service_ingest_event(
    service, definition));
  g_autofree gchar *general = acl_json(f, "general", f->member_pk, 20);
  g_assert_true(gn_communikeys_community_service_ingest_event(
    service, general));

  g_autofree gchar *original_id = NULL;
  g_autofree gchar *original =
    original_json(f, f->member_sk, f->member_pk, &original_id);
  g_assert_true(gn_communikeys_community_service_ingest_event(
    service, original));

  /* CURATOR wrapper: the wrapper author differs from the original author.
   * This is the V2 behavioral inversion — it MUST be accepted because the
   * ORIGINAL author holds the General grant. */
  g_autofree gchar *curated = target_json(
    f, original_id, f->member_pk, f->curator_sk, f->curator_pk);
  g_assert_true(gn_communikeys_community_service_ingest_event(
    service, curated));
  GListModel *targets = gn_communikeys_community_service_get_targets(
    service, f->address);
  g_assert_cmpuint(g_list_model_get_n_items(targets), ==, 1);
  g_autoptr(GnCommunikeysTargetedItem) item =
    g_list_model_get_item(targets, 0);
  g_assert_cmpstr(
    gn_communikeys_targeted_item_get_original_content(item),
    ==, "A verified long-form post");
  g_assert_cmpstr(gn_communikeys_targeted_item_get_author(item),
                  ==, f->curator_pk);

  /* A wrapper whose ORIGINAL author holds no grant is still rejected. */
  g_autofree gchar *ungranted_id = NULL;
  g_autofree gchar *ungranted =
    original_json(f, f->curator_sk, f->curator_pk, &ungranted_id);
  g_assert_true(gn_communikeys_community_service_ingest_event(
    service, ungranted));
  g_autofree gchar *rejected = target_json(
    f, ungranted_id, f->curator_pk, f->member_sk, f->member_pk);
  g_assert_false(gn_communikeys_community_service_ingest_event(
    service, rejected));
  g_assert_cmpuint(g_list_model_get_n_items(targets), ==, 1);
}

static void test_same_id_branches_are_distinct(Fixture *f,
                                               gconstpointer data) {
  (void)data;
  g_autoptr(GnCommunikeysCommunityService) service =
    gn_communikeys_community_service_new_offline(f->member_pk);
  /* Two owners publish definitions with the SAME community ID: independent
   * branches that must appear as separate rows and never merge state. */
  g_autofree gchar *branch_a = definition_json(f, 10);
  g_autofree gchar *branch_b = definition_json_for(
    f, f->owner2_sk, f->owner2_pk, f->owner2_pk, "Forked Builders", 11);
  g_assert_true(gn_communikeys_community_service_ingest_event(
    service, branch_a));
  g_assert_true(gn_communikeys_community_service_ingest_event(
    service, branch_b));
  g_assert_cmpuint(g_list_model_get_n_items(
    gn_communikeys_community_service_get_model(service)), ==, 2);

  g_autoptr(GnCommunikeysCommunityItem) first =
    gn_communikeys_community_service_lookup_community(service, f->address);
  g_autoptr(GnCommunikeysCommunityItem) second =
    gn_communikeys_community_service_lookup_community(service, f->address2);
  g_assert_nonnull(first);
  g_assert_nonnull(second);
  g_assert_cmpstr(gn_communikeys_community_item_get_name(first),
                  ==, "Cascadia Builders");
  g_assert_cmpstr(gn_communikeys_community_item_get_name(second),
                  ==, "Forked Builders");
  g_assert_cmpstr(
    gn_communikeys_community_item_get_community_id(first), ==,
    gn_communikeys_community_item_get_community_id(second));

  /* Grants stay branch-scoped: the member is granted in branch A only, so
   * an h-only chat event lands only in branch A's message store. */
  g_autofree gchar *acl = acl_json(f, "chat", f->member_pk, 20);
  g_assert_true(gn_communikeys_community_service_ingest_event(service, acl));
  g_assert_true(gn_communikeys_community_service_author_can_publish(
    service, f->address, 9, f->member_pk));
  g_assert_false(gn_communikeys_community_service_author_can_publish(
    service, f->address2, 9, f->member_pk));

  g_autofree gchar *message = exclusive_json(
    f, f->member_sk, f->member_pk, 9, "hello branch A", 30);
  g_assert_true(gn_communikeys_community_service_ingest_event(
    service, message));
  g_assert_cmpuint(g_list_model_get_n_items(
    gn_communikeys_community_service_get_messages(service, f->address)),
    ==, 1);
  g_assert_cmpuint(g_list_model_get_n_items(
    gn_communikeys_community_service_get_messages(service, f->address2)),
    ==, 0);
}

static void test_unsigned_definition_is_rejected(
    Fixture *f, gconstpointer data) {
  (void)data;
  g_autoptr(GnCommunikeysCommunityService) service =
    gn_communikeys_community_service_new_offline(f->member_pk);
  g_autoptr(NostrEvent) event = new_event(
    CAS_COMMUNITY_DEFINITION, f->owner_pk, 10, "");
  add_tag(event, nostr_tag_new("d", f->community_id, NULL));
  add_tag(event, nostr_tag_new("name", "Unsigned", NULL));
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
  g_test_add("/communikeys/service/multi-shard-union",
             Fixture, NULL, fixture_setup,
             test_multi_shard_union, fixture_teardown);
  g_test_add("/communikeys/service/curator-target",
             Fixture, NULL, fixture_setup,
             test_curator_targeted_publication, fixture_teardown);
  g_test_add("/communikeys/service/same-id-branches",
             Fixture, NULL, fixture_setup,
             test_same_id_branches_are_distinct, fixture_teardown);
  g_test_add("/communikeys/service/unsigned-rejected",
             Fixture, NULL, fixture_setup,
             test_unsigned_definition_is_rejected, fixture_teardown);
  return g_test_run();
}
