#include "nip_communikeys.h"

/* The tests perform real work inside assert(); keep them active even when the
 * enclosing build is Release (-DNDEBUG). */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Real signer pubkeys (by position). */
#define PK_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define PK_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define PK_C "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
#define PK_E "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"
/* Opaque community IDs (by position). */
#define ID_D "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
#define ID_F "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"

#define ADDR_AD "32222:" PK_A ":" ID_D
#define ADDR_BD "32222:" PK_B ":" ID_D
#define ADDR_BF "32222:" PK_B ":" ID_F

static void add_tag(NostrEvent *event, NostrTag *tag) {
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(event);
    if (!tags) {
        tags = nostr_tags_new(0);
        nostr_event_set_tags(event, tags);
    }
    nostr_tags_append(tags, tag);
}

static NostrEvent *event_new(int kind, const char *pubkey) {
    NostrEvent *event = nostr_event_new();
    assert(event);
    nostr_event_set_kind(event, kind);
    nostr_event_set_pubkey(event, pubkey);
    nostr_event_set_content(event, "");
    return event;
}

/* A fully V2-shaped definition: owner PK_A, community ID ID_D, delegated
 * profile-list author PK_B, a sharded General section and a grant-free
 * Threads section. */
static NostrEvent *valid_definition_event(void) {
    NostrEvent *event = event_new(CAS_COMMUNITY_DEFINITION, PK_A);
    add_tag(event, nostr_tag_new("d", ID_D, NULL));
    add_tag(event, nostr_tag_new("name", "Cascadia Builders", NULL));
    add_tag(event, nostr_tag_new("description", "A community for builders", NULL));
    add_tag(event, nostr_tag_new("picture", "https://media.example/p.png", NULL));
    add_tag(event, nostr_tag_new("website", "http://example.com", NULL));
    add_tag(event, nostr_tag_new("r", "wss://relay.example", NULL));
    add_tag(event, nostr_tag_new("blossom", "https://media.example", NULL));
    add_tag(event, nostr_tag_new("mint", "https://mint.example", "cashu", NULL));
    add_tag(event, nostr_tag_new("g", "c216ne", NULL));
    add_tag(event, nostr_tag_new("service", "email-digest", PK_C,
                                 "wss://svc.example",
                                 "30078:" PK_C ":digest-handler",
                                 "wss://svc.example", NULL));
    add_tag(event, nostr_tag_new("content", "General", NULL));
    add_tag(event, nostr_tag_new("k", "1111", NULL));
    add_tag(event, nostr_tag_new("k", "11", "threads", NULL));
    add_tag(event, nostr_tag_new("a", "30000:" PK_B ":" ID_D "-general",
                                 "wss://relay.example", NULL));
    add_tag(event, nostr_tag_new("a", "30000:" PK_B ":" ID_D "-general.2", NULL));
    add_tag(event, nostr_tag_new("badge", "30009:" PK_B ":contributor", NULL));
    add_tag(event, nostr_tag_new("retention", "1111", "1000", "count", NULL));
    add_tag(event, nostr_tag_new("content", "Chat", NULL));
    add_tag(event, nostr_tag_new("k", "9", NULL));
    add_tag(event, nostr_tag_new("a", "30000:" PK_B ":" ID_D "-chat", NULL));
    add_tag(event, nostr_tag_new("content", "Threads", NULL));
    add_tag(event, nostr_tag_new("k", "30023", NULL));
    return event;
}

static void test_branch_identity(void) {
    nostr_communikeys_branch_t branch;
    assert(nostr_communikeys_branch_parse(ADDR_AD, &branch));
    assert(strcmp(branch.owner, PK_A) == 0);
    assert(strcmp(branch.community_id, ID_D) == 0);
    char *formatted = nostr_communikeys_branch_format(&branch);
    assert(formatted && strcmp(formatted, ADDR_AD) == 0);
    free(formatted);

    nostr_communikeys_branch_t other;
    assert(nostr_communikeys_branch_parse(ADDR_BD, &other));
    /* Same community ID under different owners: distinct branches. */
    assert(!nostr_communikeys_branch_equal(&branch, &other));
    assert(nostr_communikeys_branch_parse(ADDR_AD, &other) &&
           nostr_communikeys_branch_equal(&branch, &other));

    /* ownerPubkey MAY equal communityId; equality confers nothing. */
    assert(nostr_communikeys_branch_parse("32222:" ID_D ":" ID_D, &other));
    assert(strcmp(other.owner, other.community_id) == 0);

    assert(!nostr_communikeys_branch_parse("30222:" PK_A ":" ID_D, &branch));
    assert(!nostr_communikeys_branch_parse("32222:" PK_A, &branch));
    assert(!nostr_communikeys_branch_parse("32222:" PK_A ":short", &branch));
    assert(!nostr_communikeys_branch_parse(
        "32222:" PK_A ":DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD",
        &branch));
    assert(!nostr_communikeys_branch_parse(PK_A, &branch));
}

static void test_section_identifier_grammar(void) {
    char *purpose = NULL;
    int shard = 0;

    assert(nostr_communikeys_section_identifier_parse(
        ID_D "-general", ID_D, &purpose, &shard));
    assert(strcmp(purpose, "general") == 0 && shard == 1);
    free(purpose);

    assert(nostr_communikeys_section_identifier_parse(
        ID_D "-general.2", ID_D, &purpose, &shard));
    assert(strcmp(purpose, "general") == 0 && shard == 2);
    free(purpose);

    /* general-2 is a purpose; general-2.2 is its second shard. */
    assert(nostr_communikeys_section_identifier_parse(
        ID_D "-general-2", ID_D, &purpose, &shard));
    assert(strcmp(purpose, "general-2") == 0 && shard == 1);
    free(purpose);

    assert(nostr_communikeys_section_identifier_parse(
        ID_D "-general-2.2", ID_D, &purpose, &shard));
    assert(strcmp(purpose, "general-2") == 0 && shard == 2);
    free(purpose);

    assert(nostr_communikeys_section_identifier_parse(
        ID_D "-room-creator.10", ID_D, &purpose, &shard));
    assert(strcmp(purpose, "room-creator") == 0 && shard == 10);
    free(purpose);

    /* Rejections. */
    assert(!nostr_communikeys_section_identifier_parse(
        ID_D "-General", ID_D, &purpose, &shard));       /* uppercase */
    assert(!nostr_communikeys_section_identifier_parse(
        ID_D "-general.1", ID_D, &purpose, &shard));     /* first shard is 2 */
    assert(!nostr_communikeys_section_identifier_parse(
        ID_D "-general.02", ID_D, &purpose, &shard));    /* non-canonical */
    assert(!nostr_communikeys_section_identifier_parse(
        ID_D "-general.", ID_D, &purpose, &shard));      /* dangling period */
    assert(!nostr_communikeys_section_identifier_parse(
        ID_D "-general.2.3", ID_D, &purpose, &shard));   /* two periods */
    assert(!nostr_communikeys_section_identifier_parse(
        ID_D "-", ID_D, &purpose, &shard));              /* empty purpose */
    assert(!nostr_communikeys_section_identifier_parse(
        ID_D "--general", ID_D, &purpose, &shard));      /* leading hyphen */
    assert(!nostr_communikeys_section_identifier_parse(
        ID_D "-general-", ID_D, &purpose, &shard));      /* trailing hyphen */
    assert(!nostr_communikeys_section_identifier_parse(
        ID_F "-general", ID_D, &purpose, &shard));       /* wrong community */
    assert(!nostr_communikeys_section_identifier_parse(
        "communikeys::tenant::group::General", ID_D, &purpose, &shard));

    /* > 200 bytes total is invalid. */
    char big[256];
    snprintf(big, sizeof(big), "%s-%0*d", ID_D, 140, 1);
    assert(!nostr_communikeys_section_identifier_parse(big, ID_D, &purpose, &shard));
}

static void test_definition_parse_build_and_resolve(void) {
    NostrEvent *event = valid_definition_event();
    assert(nostr_communikeys_definition_validate_event(event) ==
           NOSTR_COMMUNIKEYS_OK);

    nostr_communikeys_definition_t definition;
    assert(nostr_communikeys_definition_parse(event, &definition));
    assert(definition.valid);
    assert(strcmp(definition.branch.owner, PK_A) == 0);
    assert(strcmp(definition.branch.community_id, ID_D) == 0);
    assert(strcmp(definition.name, "Cascadia Builders") == 0);
    assert(strcmp(definition.description, "A community for builders") == 0);
    assert(strcmp(definition.picture, "https://media.example/p.png") == 0);
    assert(strcmp(definition.website, "http://example.com") == 0);
    assert(definition.relays_len == 1);
    assert(definition.blossom_servers_len == 1);
    assert(definition.mints_len == 1 &&
           strcmp(definition.mints[0].protocol, "cashu") == 0);
    assert(definition.services_len == 1 &&
           strcmp(definition.services[0].name, "email-digest") == 0);
    assert(definition.sections_len == 3);

    const nostr_communikeys_section_t *general =
        nostr_communikeys_definition_find_section(&definition, 11, "threads");
    assert(general && strcmp(general->name, "General") == 0);
    assert(general->profile_lists_len == 2);
    assert(strcmp(general->profile_lists[0].purpose, "general") == 0);
    assert(general->profile_lists[0].shard == 1);
    assert(strcmp(general->profile_lists[1].purpose, "general") == 0);
    assert(general->profile_lists[1].shard == 2);
    assert(general->badges_len == 1);
    assert(general->retention_len == 1 &&
           general->retention[0].kind == 1111 &&
           general->retention[0].value == 1000 &&
           general->retention[0].type == NOSTR_COMMUNIKEYS_RETENTION_COUNT);
    assert(!nostr_communikeys_definition_find_section(&definition, 11, NULL));

    /* A grant-free (zero `a`) section parses valid. */
    const nostr_communikeys_section_t *threads =
        nostr_communikeys_definition_find_section(&definition, 30023, NULL);
    assert(threads && strcmp(threads->name, "Threads") == 0);
    assert(threads->profile_lists_len == 0);

    NostrEvent *rebuilt =
        nostr_communikeys_definition_to_event(&definition, 1234);
    assert(rebuilt);
    assert(nostr_event_get_kind(rebuilt) == CAS_COMMUNITY_DEFINITION);
    assert(nostr_event_get_created_at(rebuilt) == 1234);
    assert(nostr_communikeys_definition_validate_event(rebuilt) ==
           NOSTR_COMMUNIKEYS_OK);

    /* Round-trip: the rebuilt event parses to the same shape. */
    nostr_communikeys_definition_t reparsed;
    assert(nostr_communikeys_definition_parse(rebuilt, &reparsed));
    assert(reparsed.valid);
    assert(reparsed.sections_len == 3);
    assert(strcmp(reparsed.name, definition.name) == 0);
    nostr_communikeys_definition_clear(&reparsed);

    nostr_event_free(rebuilt);
    nostr_communikeys_definition_clear(&definition);
    nostr_event_free(event);
}

static void expect_definition_invalid(NostrEvent *event,
                                      nostr_communikeys_status_t status) {
    nostr_communikeys_definition_t definition;
    assert(nostr_communikeys_definition_parse(event, &definition));
    assert(!definition.valid);
    assert(definition.validation_status == status);
    nostr_communikeys_definition_clear(&definition);
    nostr_event_free(event);
}

static void test_definition_grammar_table(void) {
    /* Missing name -> ERR_MISSING_NAME. */
    NostrEvent *event = event_new(CAS_COMMUNITY_DEFINITION, PK_A);
    add_tag(event, nostr_tag_new("d", ID_D, NULL));
    add_tag(event, nostr_tag_new("r", "wss://relay.example", NULL));
    add_tag(event, nostr_tag_new("content", "Chat", NULL));
    add_tag(event, nostr_tag_new("k", "9", NULL));
    expect_definition_invalid(event, NOSTR_COMMUNIKEYS_ERR_MISSING_NAME);

    /* A community-identifying h tag invalidates (rule 4). */
    event = valid_definition_event();
    add_tag(event, nostr_tag_new("h", ID_D, NULL));
    expect_definition_invalid(event, NOSTR_COMMUNIKEYS_ERR_BAD_TAG);

    /* Missing d. */
    event = event_new(CAS_COMMUNITY_DEFINITION, PK_A);
    add_tag(event, nostr_tag_new("name", "No d", NULL));
    add_tag(event, nostr_tag_new("r", "wss://relay.example", NULL));
    add_tag(event, nostr_tag_new("content", "Chat", NULL));
    add_tag(event, nostr_tag_new("k", "9", NULL));
    expect_definition_invalid(event, NOSTR_COMMUNIKEYS_ERR_CARDINALITY);

    /* Non-hex d. */
    event = event_new(CAS_COMMUNITY_DEFINITION, PK_A);
    add_tag(event, nostr_tag_new("d", "not-a-community-id", NULL));
    add_tag(event, nostr_tag_new("name", "Bad d", NULL));
    add_tag(event, nostr_tag_new("r", "wss://relay.example", NULL));
    add_tag(event, nostr_tag_new("content", "Chat", NULL));
    add_tag(event, nostr_tag_new("k", "9", NULL));
    expect_definition_invalid(event, NOSTR_COMMUNIKEYS_ERR_CARDINALITY);

    /* Section-local tag before the first content invalidates. */
    event = event_new(CAS_COMMUNITY_DEFINITION, PK_A);
    add_tag(event, nostr_tag_new("d", ID_D, NULL));
    add_tag(event, nostr_tag_new("name", "Order", NULL));
    add_tag(event, nostr_tag_new("r", "wss://relay.example", NULL));
    add_tag(event, nostr_tag_new("k", "9", NULL));
    add_tag(event, nostr_tag_new("content", "Chat", NULL));
    add_tag(event, nostr_tag_new("k", "9", NULL));
    expect_definition_invalid(event, NOSTR_COMMUNIKEYS_ERR_SECTION_ORDER);

    /* Recognized top-level tag inside a section invalidates. */
    event = event_new(CAS_COMMUNITY_DEFINITION, PK_A);
    add_tag(event, nostr_tag_new("d", ID_D, NULL));
    add_tag(event, nostr_tag_new("name", "Order", NULL));
    add_tag(event, nostr_tag_new("r", "wss://relay.example", NULL));
    add_tag(event, nostr_tag_new("content", "Chat", NULL));
    add_tag(event, nostr_tag_new("k", "9", NULL));
    add_tag(event, nostr_tag_new("description", "interrupts", NULL));
    expect_definition_invalid(event, NOSTR_COMMUNIKEYS_ERR_SECTION_ORDER);

    /* Duplicate case-folded section names invalidate. */
    event = event_new(CAS_COMMUNITY_DEFINITION, PK_A);
    add_tag(event, nostr_tag_new("d", ID_D, NULL));
    add_tag(event, nostr_tag_new("name", "Names", NULL));
    add_tag(event, nostr_tag_new("r", "wss://relay.example", NULL));
    add_tag(event, nostr_tag_new("content", "Chat", NULL));
    add_tag(event, nostr_tag_new("k", "9", NULL));
    add_tag(event, nostr_tag_new("content", "CHAT", NULL));
    add_tag(event, nostr_tag_new("k", "11", NULL));
    expect_definition_invalid(event, NOSTR_COMMUNIKEYS_ERR_SECTION_NAME);

    /* Duplicate exact (kind, subtype) assignment across sections. */
    event = event_new(CAS_COMMUNITY_DEFINITION, PK_A);
    add_tag(event, nostr_tag_new("d", ID_D, NULL));
    add_tag(event, nostr_tag_new("name", "Dup", NULL));
    add_tag(event, nostr_tag_new("r", "wss://relay.example", NULL));
    add_tag(event, nostr_tag_new("content", "Chat", NULL));
    add_tag(event, nostr_tag_new("k", "9", NULL));
    add_tag(event, nostr_tag_new("content", "More", NULL));
    add_tag(event, nostr_tag_new("k", "9", NULL));
    expect_definition_invalid(event, NOSTR_COMMUNIKEYS_ERR_DUPLICATE_ASSIGNMENT);

    /* A section `a` whose identifier is not section-scoped to THIS community. */
    event = event_new(CAS_COMMUNITY_DEFINITION, PK_A);
    add_tag(event, nostr_tag_new("d", ID_D, NULL));
    add_tag(event, nostr_tag_new("name", "Scope", NULL));
    add_tag(event, nostr_tag_new("r", "wss://relay.example", NULL));
    add_tag(event, nostr_tag_new("content", "Chat", NULL));
    add_tag(event, nostr_tag_new("k", "9", NULL));
    add_tag(event, nostr_tag_new("a", "30000:" PK_B ":" ID_F "-chat", NULL));
    expect_definition_invalid(event, NOSTR_COMMUNIKEYS_ERR_BAD_SECTION_IDENTIFIER);

    /* Relays are wss:// only under V2 (ws:// no longer accepted). */
    event = event_new(CAS_COMMUNITY_DEFINITION, PK_A);
    add_tag(event, nostr_tag_new("d", ID_D, NULL));
    add_tag(event, nostr_tag_new("name", "Relay", NULL));
    add_tag(event, nostr_tag_new("r", "ws://insecure.example", NULL));
    add_tag(event, nostr_tag_new("content", "Chat", NULL));
    add_tag(event, nostr_tag_new("k", "9", NULL));
    expect_definition_invalid(event, NOSTR_COMMUNIKEYS_ERR_BAD_METADATA);

    /* picture must be HTTPS. */
    event = event_new(CAS_COMMUNITY_DEFINITION, PK_A);
    add_tag(event, nostr_tag_new("d", ID_D, NULL));
    add_tag(event, nostr_tag_new("name", "Pic", NULL));
    add_tag(event, nostr_tag_new("picture", "http://media.example/p.png", NULL));
    add_tag(event, nostr_tag_new("r", "wss://relay.example", NULL));
    add_tag(event, nostr_tag_new("content", "Chat", NULL));
    add_tag(event, nostr_tag_new("k", "9", NULL));
    expect_definition_invalid(event, NOSTR_COMMUNIKEYS_ERR_BAD_METADATA);

    /* Definition content must be empty. */
    event = valid_definition_event();
    nostr_event_set_content(event, "not empty");
    expect_definition_invalid(event, NOSTR_COMMUNIKEYS_ERR_BAD_CONTENT);

    /* Unknown tags are preserved extensions and do not invalidate. */
    event = valid_definition_event();
    add_tag(event, nostr_tag_new("x-custom", "extension", NULL));
    assert(nostr_communikeys_definition_validate_event(event) ==
           NOSTR_COMMUNIKEYS_OK);
    nostr_event_free(event);
}

static NostrEvent *profile_list_event(const char *author, const char *d,
                                      const char *member, int64_t created_at) {
    NostrEvent *event = event_new(NOSTR_COMMUNIKEYS_KIND_PROFILE_LIST, author);
    nostr_event_set_created_at(event, created_at);
    add_tag(event, nostr_tag_new("d", d, NULL));
    if (member) add_tag(event, nostr_tag_new("p", member, NULL));
    return event;
}

static void test_multi_shard_union_authorization(void) {
    NostrEvent *definition_event = valid_definition_event();
    nostr_communikeys_definition_t definition;
    assert(nostr_communikeys_definition_parse(definition_event, &definition));
    assert(definition.valid);

    /* Delegated author PK_B signs both General shards; PK_C appears only in
     * shard .2. */
    NostrEvent *shard1 =
        profile_list_event(PK_B, ID_D "-general", PK_E, 10);
    NostrEvent *shard2 =
        profile_list_event(PK_B, ID_D "-general.2", PK_C, 10);
    const NostrEvent *lists[] = {shard1, shard2};

    /* Member listed only in shard .2 is granted via the union. */
    assert(nostr_communikeys_author_can_publish(
        &definition, 1111, NULL, lists, 2, PK_C));
    assert(nostr_communikeys_author_can_publish(
        &definition, 1111, NULL, lists, 2, PK_E));
    /* The owner retains inherent authority with zero evidence. */
    assert(nostr_communikeys_author_can_publish(
        &definition, 1111, NULL, NULL, 0, PK_A));
    /* A referenced delegated list author holds the structural role. */
    assert(nostr_communikeys_author_can_publish(
        &definition, 1111, NULL, NULL, 0, PK_B));
    /* Grant-free section: no p-tag grants, but not an error. */
    assert(!nostr_communikeys_author_can_publish(
        &definition, 30023, NULL, lists, 2, PK_C));
    assert(nostr_communikeys_author_can_publish(
        &definition, 30023, NULL, NULL, 0, PK_A));
    /* Unknown kind resolves no section. */
    assert(!nostr_communikeys_author_can_publish(
        &definition, 4242, NULL, lists, 2, PK_A));

    /* Deterministic replacement: a newer shard .2 without PK_C revokes. */
    NostrEvent *shard2_newer =
        profile_list_event(PK_B, ID_D "-general.2", NULL, 20);
    const NostrEvent *lists_replaced[] = {shard1, shard2, shard2_newer};
    assert(!nostr_communikeys_author_can_publish(
        &definition, 1111, NULL, lists_replaced, 3, PK_C));
    assert(nostr_communikeys_author_can_publish(
        &definition, 1111, NULL, lists_replaced, 3, PK_E));

    /* A list signed by someone other than the referenced author is ignored. */
    NostrEvent *forged =
        profile_list_event(PK_E, ID_D "-chat", PK_C, 30);
    const NostrEvent *forged_lists[] = {forged};
    assert(!nostr_communikeys_author_can_publish(
        &definition, 9, NULL, forged_lists, 1, PK_C));

    nostr_event_free(forged);
    nostr_event_free(shard2_newer);
    nostr_event_free(shard1);
    nostr_event_free(shard2);
    nostr_communikeys_definition_clear(&definition);
    nostr_event_free(definition_event);
}

static void make_hex_id(char out[65], unsigned v) {
    static const char hexd[] = "0123456789abcdef";
    for (size_t i = 0; i < 60; ++i) out[i] = '0';
    out[60] = hexd[(v >> 12) & 0xF];
    out[61] = hexd[(v >> 8) & 0xF];
    out[62] = hexd[(v >> 4) & 0xF];
    out[63] = hexd[v & 0xF];
    out[64] = '\0';
}

static void add_pair(NostrEvent *event, const char *owner, const char *id,
                     const char *relay) {
    char address[6 + 64 + 1 + 64 + 1];
    snprintf(address, sizeof(address), "32222:%s:%s", owner, id);
    add_tag(event, nostr_tag_new("h", id, NULL));
    add_tag(event, relay
        ? nostr_tag_new("a", address, relay, NULL)
        : nostr_tag_new("a", address, NULL));
}

static NostrEvent *wrapper_base(const char *curator, const char *reference) {
    NostrEvent *event = event_new(CAS_TARGETED_PUBLICATION, curator);
    add_tag(event, nostr_tag_new("d", "route-1", NULL));
    if (reference)
        add_tag(event, nostr_tag_new("a", reference, "wss://source.example", NULL));
    add_tag(event, nostr_tag_new("k", "30023", NULL));
    return event;
}

static void expect_wrapper_status(NostrEvent *event,
                                  nostr_communikeys_status_t status) {
    nostr_communikeys_targeted_publication_t parsed;
    assert(nostr_communikeys_targeted_publication_parse(event, &parsed) == status);
    if (status == NOSTR_COMMUNIKEYS_OK)
        nostr_communikeys_targeted_publication_clear(&parsed);
    nostr_event_free(event);
}

static void test_wrapper_pair_state_machine(void) {
    /* Curator wrapper with two adjacent pairs — including two branches that
     * share a community ID, which is explicitly allowed. */
    NostrEvent *event = wrapper_base(PK_C, "30023:" PK_E ":post-1");
    add_pair(event, PK_A, ID_D, "wss://one.example");
    add_pair(event, PK_B, ID_D, NULL);
    nostr_communikeys_targeted_publication_t parsed;
    assert(nostr_communikeys_targeted_publication_parse(event, &parsed) ==
           NOSTR_COMMUNIKEYS_OK);
    assert(strcmp(parsed.curator, PK_C) == 0);
    assert(parsed.has_source);
    assert(parsed.reference_type == NOSTR_COMMUNIKEYS_REFERENCE_ADDRESS);
    assert(strcmp(parsed.reference_author, PK_E) == 0);
    assert(parsed.targets_len == 2);
    assert(strcmp(parsed.targets[0].community_id, ID_D) == 0);
    assert(strcmp(parsed.targets[0].branch.owner, PK_A) == 0);
    assert(strcmp(parsed.targets[0].relay, "wss://one.example") == 0);
    assert(strcmp(parsed.targets[1].branch.owner, PK_B) == 0);
    assert(parsed.targets[1].relay == NULL);
    nostr_communikeys_targeted_publication_clear(&parsed);
    nostr_event_free(event);

    /* Unpaired h (no following a). */
    event = wrapper_base(PK_C, "30023:" PK_E ":post-1");
    add_tag(event, nostr_tag_new("h", ID_D, NULL));
    expect_wrapper_status(event, NOSTR_COMMUNIKEYS_ERR_TARGET_PAIR);

    /* h followed by a with a mismatched identifier. */
    event = wrapper_base(PK_C, "30023:" PK_E ":post-1");
    add_tag(event, nostr_tag_new("h", ID_D, NULL));
    add_tag(event, nostr_tag_new("a", ADDR_BF, NULL));
    expect_wrapper_status(event, NOSTR_COMMUNIKEYS_ERR_TARGET_PAIR);

    /* h followed by a with kind != 32222. */
    event = wrapper_base(PK_C, "30023:" PK_E ":post-1");
    add_tag(event, nostr_tag_new("h", ID_D, NULL));
    add_tag(event, nostr_tag_new("a", "30222:" PK_A ":" ID_D, NULL));
    expect_wrapper_status(event, NOSTR_COMMUNIKEYS_ERR_TARGET_PAIR);

    /* Interleaved non-adjacent pair: h, then an unrelated tag, then a. */
    event = wrapper_base(PK_C, "30023:" PK_E ":post-1");
    add_tag(event, nostr_tag_new("h", ID_D, NULL));
    add_tag(event, nostr_tag_new("t", "topic", NULL));
    add_tag(event, nostr_tag_new("a", ADDR_AD, NULL));
    expect_wrapper_status(event, NOSTR_COMMUNIKEYS_ERR_TARGET_PAIR);

    /* Duplicate definition address. */
    event = wrapper_base(PK_C, "30023:" PK_E ":post-1");
    add_pair(event, PK_A, ID_D, NULL);
    add_pair(event, PK_A, ID_D, NULL);
    expect_wrapper_status(event, NOSTR_COMMUNIKEYS_ERR_TARGET_PAIR);

    /* Zero pairs. */
    event = wrapper_base(PK_C, "30023:" PK_E ":post-1");
    expect_wrapper_status(event, NOSTR_COMMUNIKEYS_ERR_BAD_TARGETS);

    /* Thirteen pairs. */
    event = wrapper_base(PK_C, "30023:" PK_E ":post-1");
    for (unsigned i = 0; i < 13; ++i) {
        char id[65];
        make_hex_id(id, i + 1);
        add_pair(event, PK_A, id, NULL);
    }
    expect_wrapper_status(event, NOSTR_COMMUNIKEYS_ERR_BAD_TARGETS);

    /* Twelve pairs is the maximum and valid. */
    event = wrapper_base(PK_C, "30023:" PK_E ":post-1");
    for (unsigned i = 0; i < 12; ++i) {
        char id[65];
        make_hex_id(id, i + 1);
        add_pair(event, PK_A, id, NULL);
    }
    expect_wrapper_status(event, NOSTR_COMMUNIKEYS_OK);

    /* Any p tag is rejected outright (V1-shaped wrappers cannot slip in). */
    event = wrapper_base(PK_C, "30023:" PK_E ":post-1");
    add_pair(event, PK_A, ID_D, NULL);
    add_tag(event, nostr_tag_new("p", PK_B, NULL));
    expect_wrapper_status(event, NOSTR_COMMUNIKEYS_ERR_BAD_TARGETS);
}

static void test_wrapper_source_forms(void) {
    /* Address source with kind mismatch fails. */
    NostrEvent *event = event_new(CAS_TARGETED_PUBLICATION, PK_C);
    add_tag(event, nostr_tag_new("d", "route-1", NULL));
    add_tag(event, nostr_tag_new("a", "31922:" PK_E ":cal-1", NULL));
    add_tag(event, nostr_tag_new("k", "30023", NULL));
    add_pair(event, PK_A, ID_D, NULL);
    expect_wrapper_status(event, NOSTR_COMMUNIKEYS_ERR_BAD_REFERENCE);

    /* e source whose author hint differs from the curator MUST pass. */
    event = event_new(CAS_TARGETED_PUBLICATION, PK_C);
    add_tag(event, nostr_tag_new("d", "route-1", NULL));
    add_tag(event, nostr_tag_new("e", ID_F, "", PK_E, NULL));
    add_tag(event, nostr_tag_new("k", "1", NULL));
    add_pair(event, PK_A, ID_D, NULL);
    nostr_communikeys_targeted_publication_t parsed;
    assert(nostr_communikeys_targeted_publication_parse(event, &parsed) ==
           NOSTR_COMMUNIKEYS_OK);
    assert(parsed.has_source);
    assert(parsed.reference_type == NOSTR_COMMUNIKEYS_REFERENCE_EVENT);
    assert(strcmp(parsed.reference_author, PK_E) == 0);
    assert(strcmp(parsed.curator, PK_C) == 0);
    nostr_communikeys_targeted_publication_clear(&parsed);
    nostr_event_free(event);

    /* Two sources fail (closed grammar). */
    event = event_new(CAS_TARGETED_PUBLICATION, PK_C);
    add_tag(event, nostr_tag_new("d", "route-1", NULL));
    add_tag(event, nostr_tag_new("e", ID_F, NULL));
    add_tag(event, nostr_tag_new("a", "30023:" PK_E ":post-1", NULL));
    add_tag(event, nostr_tag_new("k", "30023", NULL));
    add_pair(event, PK_A, ID_D, NULL);
    expect_wrapper_status(event, NOSTR_COMMUNIKEYS_ERR_BAD_REFERENCE);

    /* Sourceless wrapper is valid. */
    event = event_new(CAS_TARGETED_PUBLICATION, PK_C);
    add_tag(event, nostr_tag_new("d", "targeting-7", NULL));
    add_tag(event, nostr_tag_new("k", "1", NULL));
    add_pair(event, PK_A, ID_D, NULL);
    assert(nostr_communikeys_targeted_publication_parse(event, &parsed) ==
           NOSTR_COMMUNIKEYS_OK);
    assert(!parsed.has_source);
    nostr_communikeys_targeted_publication_clear(&parsed);

    /* Sourceless validation: the original carries h == wrapper d and shares
     * the wrapper author. */
    NostrEvent *original = event_new(1, PK_C);
    add_tag(original, nostr_tag_new("h", "targeting-7", NULL));
    assert(nostr_communikeys_targeted_publication_validate(event, original) ==
           NOSTR_COMMUNIKEYS_OK);
    nostr_event_free(original);

    original = event_new(1, PK_E); /* different author fails sourceless form */
    add_tag(original, nostr_tag_new("h", "targeting-7", NULL));
    assert(nostr_communikeys_targeted_publication_validate(event, original) ==
           NOSTR_COMMUNIKEYS_ERR_AUTHOR_MISMATCH);
    nostr_event_free(original);
    nostr_event_free(event);
}

static void test_curator_wrapper_round_trip(void) {
    /* Original authored by PK_E; wrapper curated by PK_C. */
    NostrEvent *original = event_new(30023, PK_E);
    add_tag(original, nostr_tag_new("d", "post-1", NULL));
    nostr_communikeys_community_target_t targets[2];
    memset(targets, 0, sizeof(targets));
    memcpy(targets[0].community_id, ID_D, 65);
    assert(nostr_communikeys_branch_parse(ADDR_AD, &targets[0].branch));
    targets[0].relay = "wss://one.example";
    memcpy(targets[1].community_id, ID_F, 65);
    assert(nostr_communikeys_branch_parse(ADDR_BF, &targets[1].branch));

    nostr_communikeys_targeted_publication_t publication = {
        .identifier = "route-1",
        .has_source = true,
        .reference_type = NOSTR_COMMUNIKEYS_REFERENCE_ADDRESS,
        .reference = "30023:" PK_E ":post-1",
        .reference_relay = "wss://source.example",
        .reference_author = PK_E,
        .original_kind = 30023,
        .curator = PK_C,
        .targets = targets,
        .targets_len = 2
    };
    NostrEvent *event =
        nostr_communikeys_targeted_publication_to_event(&publication, 99);
    assert(event);
    assert(strcmp(nostr_event_get_pubkey(event), PK_C) == 0);

    nostr_communikeys_targeted_publication_t parsed;
    assert(nostr_communikeys_targeted_publication_parse(event, &parsed) ==
           NOSTR_COMMUNIKEYS_OK);
    assert(parsed.targets_len == 2);
    assert(strcmp(parsed.targets[0].relay, "wss://one.example") == 0);
    assert(parsed.targets[1].relay == NULL);
    nostr_communikeys_targeted_publication_clear(&parsed);

    /* Curator != original author validates cleanly against the original. */
    assert(nostr_communikeys_targeted_publication_validate(event, original) ==
           NOSTR_COMMUNIKEYS_OK);
    /* Kind mismatch is still a genuine consistency failure. */
    nostr_event_set_kind(original, 1);
    assert(nostr_communikeys_targeted_publication_validate(event, original) ==
           NOSTR_COMMUNIKEYS_ERR_REFERENCE_MISMATCH);
    nostr_event_free(original);
    nostr_event_free(event);

    /* Event-source round trip against a computed event ID. */
    original = event_new(1, PK_E);
    char *original_id = nostr_event_get_id(original);
    assert(original_id);
    nostr_communikeys_targeted_publication_t by_event = {
        .identifier = "route-2",
        .has_source = true,
        .reference_type = NOSTR_COMMUNIKEYS_REFERENCE_EVENT,
        .reference = original_id,
        .reference_relay = NULL,
        .reference_author = PK_E,
        .original_kind = 1,
        .curator = PK_C,
        .targets = targets,
        .targets_len = 1
    };
    event = nostr_communikeys_targeted_publication_to_event(&by_event, 100);
    assert(event);
    assert(nostr_communikeys_targeted_publication_validate(event, original) ==
           NOSTR_COMMUNIKEYS_OK);
    nostr_event_free(event);
    free(original_id);
    nostr_event_free(original);
}

static void test_exclusive_h(void) {
    NostrEvent *event = event_new(9, PK_B);
    assert(nostr_communikeys_exclusive_add_h(event, ID_D));
    char community[65];
    assert(nostr_communikeys_exclusive_validate(event, community) ==
           NOSTR_COMMUNIKEYS_OK);
    assert(strcmp(community, ID_D) == 0);
    assert(!nostr_communikeys_exclusive_add_h(event, ID_F));
    add_tag(event, nostr_tag_new("h", ID_F, NULL));
    assert(nostr_communikeys_exclusive_validate(event, NULL) ==
           NOSTR_COMMUNIKEYS_ERR_CARDINALITY);
    nostr_event_free(event);

    event = event_new(1, PK_B);
    assert(!nostr_communikeys_exclusive_add_h(event, ID_D));
    assert(nostr_communikeys_exclusive_validate(event, NULL) ==
           NOSTR_COMMUNIKEYS_ERR_WRONG_KIND);
    nostr_event_free(event);
}

static void test_pointer_round_trip(void) {
    nostr_communikeys_pointer_t pointer;
    memset(&pointer, 0, sizeof(pointer));
    assert(nostr_communikeys_branch_parse(ADDR_AD, &pointer.branch));

    /* Zero hints. */
    char *naddr = nostr_communikeys_pointer_format(&pointer);
    assert(naddr && strncmp(naddr, "naddr1", 6) == 0);
    nostr_communikeys_pointer_t parsed;
    assert(nostr_communikeys_pointer_parse(naddr, &parsed) ==
           NOSTR_COMMUNIKEYS_OK);
    assert(nostr_communikeys_branch_equal(&parsed.branch, &pointer.branch));
    assert(parsed.relays_len == 0);
    nostr_communikeys_pointer_clear(&parsed);
    free(naddr);

    /* One hint. */
    char *one[] = {(char *)"wss://one.example"};
    pointer.relays = one;
    pointer.relays_len = 1;
    naddr = nostr_communikeys_pointer_format(&pointer);
    assert(naddr);
    assert(nostr_communikeys_pointer_parse(naddr, &parsed) ==
           NOSTR_COMMUNIKEYS_OK);
    assert(parsed.relays_len == 1 &&
           strcmp(parsed.relays[0], "wss://one.example") == 0);
    nostr_communikeys_pointer_clear(&parsed);
    free(naddr);

    /* Four declared hints with one duplicate: emission keeps the first three
     * unique hints in declared order. */
    char *four[] = {(char *)"wss://one.example", (char *)"wss://two.example",
                    (char *)"wss://one.example", (char *)"wss://three.example"};
    pointer.relays = four;
    pointer.relays_len = 4;
    naddr = nostr_communikeys_pointer_format(&pointer);
    assert(naddr);
    assert(nostr_communikeys_pointer_parse(naddr, &parsed) ==
           NOSTR_COMMUNIKEYS_OK);
    assert(parsed.relays_len == 3);
    assert(strcmp(parsed.relays[0], "wss://one.example") == 0);
    assert(strcmp(parsed.relays[1], "wss://two.example") == 0);
    assert(strcmp(parsed.relays[2], "wss://three.example") == 0);
    /* Pointer equality ignores hints: the branch survives. */
    assert(nostr_communikeys_branch_equal(&parsed.branch, &pointer.branch));
    nostr_communikeys_pointer_clear(&parsed);

    /* "nostr:" prefixed pointers parse too. */
    size_t prefixed_len = strlen(naddr) + 7;
    char *prefixed = malloc(prefixed_len);
    assert(prefixed);
    snprintf(prefixed, prefixed_len, "nostr:%s", naddr);
    assert(nostr_communikeys_pointer_parse(prefixed, &parsed) ==
           NOSTR_COMMUNIKEYS_OK);
    nostr_communikeys_pointer_clear(&parsed);
    free(prefixed);
    free(naddr);

    /* Non-wss hints are rejected on emission. */
    char *insecure[] = {(char *)"ws://local:8080"};
    pointer.relays = insecure;
    pointer.relays_len = 1;
    assert(nostr_communikeys_pointer_format(&pointer) == NULL);

    /* Garbage and non-naddr strings fail. */
    assert(nostr_communikeys_pointer_parse("ncommunity://" PK_A, &parsed) ==
           NOSTR_COMMUNIKEYS_ERR_BAD_BRANCH);
    assert(nostr_communikeys_pointer_parse(PK_A, &parsed) ==
           NOSTR_COMMUNIKEYS_ERR_BAD_BRANCH);
}

static void test_coordinates(void) {
    nostr_communikeys_coordinate_t coordinate;
    assert(nostr_communikeys_coordinate_parse(
        "30000:" PK_A ":" ID_D "-general", "wss://relay.example", &coordinate));
    assert(coordinate.kind == 30000);
    assert(strcmp(coordinate.identifier, ID_D "-general") == 0);
    char *value = nostr_communikeys_coordinate_format(&coordinate);
    assert(value && strcmp(value, "30000:" PK_A ":" ID_D "-general") == 0);
    free(value);
    nostr_communikeys_coordinate_clear(&coordinate);
    assert(!nostr_communikeys_coordinate_parse(
        "30000:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA:x",
        NULL, &coordinate));
    assert(!nostr_communikeys_coordinate_parse("30000:" PK_A, NULL, &coordinate));
    assert(!nostr_communikeys_coordinate_parse("030000:" PK_A ":x", NULL, &coordinate));
}

static void test_profile_list_parse(void) {
    NostrEvent *list = profile_list_event(PK_B, ID_D "-chat", PK_C, 5);
    add_tag(list, nostr_tag_new("p", PK_C, "duplicate ignored", NULL));
    add_tag(list, nostr_tag_new("p", "malformed", NULL)); /* ignored */

    nostr_communikeys_profile_list_t parsed;
    assert(nostr_communikeys_profile_list_parse(list, PK_B, ID_D "-chat",
                                                &parsed) == NOSTR_COMMUNIKEYS_OK);
    assert(parsed.members_len == 1);
    assert(nostr_communikeys_profile_list_contains(&parsed, PK_C));
    assert(!nostr_communikeys_profile_list_contains(&parsed, PK_E));
    nostr_communikeys_profile_list_clear(&parsed);

    assert(nostr_communikeys_profile_list_parse(list, PK_E, ID_D "-chat",
                                                &parsed) ==
           NOSTR_COMMUNIKEYS_ERR_AUTHOR_MISMATCH);
    assert(nostr_communikeys_profile_list_parse(list, PK_B, ID_D "-general",
                                                &parsed) ==
           NOSTR_COMMUNIKEYS_ERR_REFERENCE_MISMATCH);
    nostr_event_free(list);
}

/* The exact events from cascadia-nips examples/community-definition.json and
 * examples/targeted-publication.json (the normative Item-1 reference). */
#define EX_OWNER "1111111111111111111111111111111111111111111111111111111111111111"
#define EX_ID    "5555555555555555555555555555555555555555555555555555555555555555"
#define EX_LIST2 "6666666666666666666666666666666666666666666666666666666666666666"
#define EX_SVC   "7777777777777777777777777777777777777777777777777777777777777777"
#define EX_CUR   "8888888888888888888888888888888888888888888888888888888888888888"
#define EX_ID2   "9999999999999999999999999999999999999999999999999999999999999999"
#define EX_OWN2  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define EX_TOS   "3333333333333333333333333333333333333333333333333333333333333333"
#define EX_SRC   "2222222222222222222222222222222222222222222222222222222222222222"

static void test_item1_canonical_examples(void) {
    /* Definition example. */
    NostrEvent *event = event_new(CAS_COMMUNITY_DEFINITION, EX_OWNER);
    nostr_event_set_created_at(event, 1786089600);
    add_tag(event, nostr_tag_new("d", EX_ID, NULL));
    add_tag(event, nostr_tag_new("name", "Cascadia Builders", NULL));
    add_tag(event, nostr_tag_new("description", "A community for Cascadia builders", NULL));
    add_tag(event, nostr_tag_new("picture", "https://example.com/picture.png", NULL));
    add_tag(event, nostr_tag_new("website", "https://example.com", NULL));
    add_tag(event, nostr_tag_new("r", "wss://relay.example", NULL));
    add_tag(event, nostr_tag_new("blossom", "https://blossom.example", NULL));
    add_tag(event, nostr_tag_new("grasp", "wss://grasp.example", NULL));
    add_tag(event, nostr_tag_new("mint", "https://mint.example", "cashu", NULL));
    add_tag(event, nostr_tag_new("location", "Cascadia", NULL));
    add_tag(event, nostr_tag_new("g", "c23", NULL));
    add_tag(event, nostr_tag_new("tos", EX_TOS, "wss://relay.example", NULL));
    add_tag(event, nostr_tag_new("service", "email-digest", EX_SVC,
                                 "wss://svc-request.example",
                                 "30078:" EX_SVC ":email-digest",
                                 "wss://svc-handler.example", NULL));
    add_tag(event, nostr_tag_new("content", "General", NULL));
    add_tag(event, nostr_tag_new("k", "1111", NULL));
    add_tag(event, nostr_tag_new("k", "7", NULL));
    add_tag(event, nostr_tag_new("k", "1985", NULL));
    add_tag(event, nostr_tag_new("a", "30000:" EX_OWNER ":" EX_ID "-general",
                                 "wss://relay.example", NULL));
    add_tag(event, nostr_tag_new("a", "30000:" EX_OWNER ":" EX_ID "-general.2",
                                 "wss://relay.example", NULL));
    add_tag(event, nostr_tag_new("content", "Chat", NULL));
    add_tag(event, nostr_tag_new("k", "9", NULL));
    add_tag(event, nostr_tag_new("a", "30000:" EX_LIST2 ":" EX_ID "-chat",
                                 "wss://relay.example", NULL));
    add_tag(event, nostr_tag_new("content", "Threads", NULL));
    add_tag(event, nostr_tag_new("k", "11", "threads", NULL));

    nostr_communikeys_definition_t definition;
    assert(nostr_communikeys_definition_parse(event, &definition));
    assert(definition.valid);
    assert(strcmp(definition.branch.owner, EX_OWNER) == 0);
    assert(strcmp(definition.branch.community_id, EX_ID) == 0);
    assert(strcmp(definition.name, "Cascadia Builders") == 0);
    assert(definition.sections_len == 3);
    assert(definition.sections[0].profile_lists_len == 2); /* sharded */
    assert(definition.sections[2].profile_lists_len == 0); /* grant-free */
    assert(definition.services_len == 1);
    assert(definition.tos && strcmp(definition.tos, EX_TOS) == 0);

    /* Round-trip through the writer. */
    NostrEvent *rebuilt =
        nostr_communikeys_definition_to_event(&definition, 1786089600);
    assert(rebuilt);
    assert(nostr_communikeys_definition_validate_event(rebuilt) ==
           NOSTR_COMMUNIKEYS_OK);
    nostr_event_free(rebuilt);
    nostr_communikeys_definition_clear(&definition);
    nostr_event_free(event);

    /* Curator wrapper example: the wrapper author differs from the source
     * author, with two adjacent pairs. */
    event = event_new(CAS_TARGETED_PUBLICATION, EX_CUR);
    nostr_event_set_created_at(event, 1786089601);
    add_tag(event, nostr_tag_new("d", "target-6f80b6d8", NULL));
    add_tag(event, nostr_tag_new("a", "30023:" EX_SRC ":builders-roadmap",
                                 "wss://source.example", NULL));
    add_tag(event, nostr_tag_new("k", "30023", NULL));
    add_tag(event, nostr_tag_new("h", EX_ID, NULL));
    add_tag(event, nostr_tag_new("a", "32222:" EX_OWNER ":" EX_ID,
                                 "wss://community-1.example", NULL));
    add_tag(event, nostr_tag_new("h", EX_ID2, NULL));
    add_tag(event, nostr_tag_new("a", "32222:" EX_OWN2 ":" EX_ID2,
                                 "wss://community-2.example", NULL));

    nostr_communikeys_targeted_publication_t wrapper;
    assert(nostr_communikeys_targeted_publication_parse(event, &wrapper) ==
           NOSTR_COMMUNIKEYS_OK);
    assert(strcmp(wrapper.curator, EX_CUR) == 0);
    assert(wrapper.has_source);
    assert(strcmp(wrapper.reference_author, EX_SRC) == 0); /* curator model */
    assert(wrapper.targets_len == 2);
    assert(strcmp(wrapper.targets[0].branch.owner, EX_OWNER) == 0);
    assert(strcmp(wrapper.targets[1].branch.owner, EX_OWN2) == 0);

    /* Round-trip through the writer. */
    NostrEvent *rebuilt_wrapper =
        nostr_communikeys_targeted_publication_to_event(&wrapper, 1786089601);
    assert(rebuilt_wrapper);
    nostr_communikeys_targeted_publication_t reparsed;
    assert(nostr_communikeys_targeted_publication_parse(rebuilt_wrapper,
                                                        &reparsed) ==
           NOSTR_COMMUNIKEYS_OK);
    assert(reparsed.targets_len == 2);
    nostr_communikeys_targeted_publication_clear(&reparsed);
    nostr_communikeys_targeted_publication_clear(&wrapper);
    nostr_event_free(rebuilt_wrapper);
    nostr_event_free(event);
}

static void test_status_strings_cover_every_case(void) {
    for (int status = NOSTR_COMMUNIKEYS_OK;
         status <= NOSTR_COMMUNIKEYS_ERR_BAD_METADATA; ++status) {
        const char *text = nostr_communikeys_status_string(
            (nostr_communikeys_status_t)status);
        assert(text && strcmp(text, "unknown") != 0);
    }
}

int main(void) {
    test_branch_identity();
    test_section_identifier_grammar();
    test_definition_parse_build_and_resolve();
    test_definition_grammar_table();
    test_multi_shard_union_authorization();
    test_wrapper_pair_state_machine();
    test_wrapper_source_forms();
    test_curator_wrapper_round_trip();
    test_exclusive_h();
    test_pointer_round_trip();
    test_coordinates();
    test_profile_list_parse();
    test_item1_canonical_examples();
    test_status_strings_cover_every_case();
    puts("nip-communikeys ok");
    return 0;
}
