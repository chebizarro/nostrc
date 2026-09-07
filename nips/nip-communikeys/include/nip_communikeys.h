#ifndef NOSTR_NIP_COMMUNIKEYS_H
#define NOSTR_NIP_COMMUNIKEYS_H

/*
 * Communikeys V2 (NIP-CAS-0007).
 *
 * Identity model:
 *   communityId       = opaque 64-char lowercase-hex identifier (NOT a key)
 *   owner             = definition event author (a real signing pubkey)
 *   definitionAddress = "32222:<ownerPubkey>:<communityId>"
 *   canonical pointer = NIP-19 naddr(32222, owner, communityId, <=3 relay hints)
 *
 * A community ID grants no signing, person, profile, relay, or administrative
 * meaning. Implementations MUST NOT use it as an event author, person `p` tag,
 * outbox/profile lookup, DM recipient, or permission-list signer. Different
 * owners MAY publish definitions with the same community ID; those are
 * distinct branches and MUST NOT replace one another.
 *
 * This header is the V2 ABI (major 2). It is intentionally incompatible with
 * the never-adopted V1 ABI: the kind value changed (10222 -> 32222), the
 * `ncommunity://` codec is deleted in favor of naddr pointers, targeting moved
 * from `p`+`r` pubkey targets to adjacent `h`+`a` branch pairs, and the status
 * enum was renumbered.
 */

#include "nostr-event.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical NIP-CAS-0007 registry symbols (Communikeys V2). */
#ifndef CAS_COMMUNITY_DEFINITION
#define CAS_COMMUNITY_DEFINITION ((uint16_t)32222)
#endif
#ifndef CAS_TARGETED_PUBLICATION
#define CAS_TARGETED_PUBLICATION ((uint16_t)30222)
#endif

#define NOSTR_COMMUNIKEYS_KIND_PROFILE_LIST 30000
#define NOSTR_COMMUNIKEYS_KIND_BADGE_DEFINITION 30009
#define NOSTR_COMMUNIKEYS_MAX_TARGETS 12
#define NOSTR_COMMUNIKEYS_MAX_RELAY_HINTS 3 /* naddr emission cap */
#define NOSTR_COMMUNIKEYS_MAX_SECTION_D 200 /* UTF-8 bytes */
#define NOSTR_COMMUNIKEYS_MAX_RELAYS 20     /* r/blossom/grasp/mint caps */
#define NOSTR_COMMUNIKEYS_MAX_SERVICES 50

typedef enum {
    NOSTR_COMMUNIKEYS_OK = 0,
    NOSTR_COMMUNIKEYS_ERR_NULL,
    NOSTR_COMMUNIKEYS_ERR_OOM,
    NOSTR_COMMUNIKEYS_ERR_WRONG_KIND,
    NOSTR_COMMUNIKEYS_ERR_BAD_PUBKEY,
    NOSTR_COMMUNIKEYS_ERR_BAD_CONTENT,
    NOSTR_COMMUNIKEYS_ERR_BAD_TAG,
    NOSTR_COMMUNIKEYS_ERR_CARDINALITY,
    NOSTR_COMMUNIKEYS_ERR_MISSING_RELAY,
    NOSTR_COMMUNIKEYS_ERR_SECTION_ORDER,
    NOSTR_COMMUNIKEYS_ERR_SECTION_NAME,
    NOSTR_COMMUNIKEYS_ERR_SECTION_KIND,
    NOSTR_COMMUNIKEYS_ERR_SECTION_ACL,
    NOSTR_COMMUNIKEYS_ERR_DUPLICATE_ASSIGNMENT,
    NOSTR_COMMUNIKEYS_ERR_BAD_COORDINATE,
    NOSTR_COMMUNIKEYS_ERR_BAD_REFERENCE,
    NOSTR_COMMUNIKEYS_ERR_BAD_TARGETS,
    NOSTR_COMMUNIKEYS_ERR_AUTHOR_MISMATCH,
    NOSTR_COMMUNIKEYS_ERR_REFERENCE_MISMATCH,
    NOSTR_COMMUNIKEYS_ERR_BAD_BRANCH,
    NOSTR_COMMUNIKEYS_ERR_BAD_SECTION_IDENTIFIER,
    NOSTR_COMMUNIKEYS_ERR_TARGET_PAIR,
    NOSTR_COMMUNIKEYS_ERR_MISSING_NAME,
    NOSTR_COMMUNIKEYS_ERR_BAD_METADATA
} nostr_communikeys_status_t;

const char *nostr_communikeys_status_string(nostr_communikeys_status_t status);
bool nostr_communikeys_is_lower_hex_32(const char *value);

/*
 * Branch identity: the tuple (kind=32222, ownerPubkey, communityId).
 * `community_id` is opaque: it is validated as lowercase hex only and MUST
 * NOT be lifted to a curve point or used in any signer/person position.
 */
typedef struct {
    char owner[65];        /* lowercase hex, x-only */
    char community_id[65]; /* lowercase hex, OPAQUE */
} nostr_communikeys_branch_t;

/* Parses "32222:<owner>:<communityId>". */
bool nostr_communikeys_branch_parse(const char *address,
                                    nostr_communikeys_branch_t *out);
/* Formats "32222:<owner>:<communityId>"; caller frees. */
char *nostr_communikeys_branch_format(const nostr_communikeys_branch_t *branch);
bool nostr_communikeys_branch_equal(const nostr_communikeys_branch_t *a,
                                    const nostr_communikeys_branch_t *b);

typedef struct {
    int kind;
    char *pubkey;
    char *identifier;
    char *relay;
} nostr_communikeys_coordinate_t;

bool nostr_communikeys_coordinate_parse(const char *value,
                                        const char *relay,
                                        nostr_communikeys_coordinate_t *out);
char *nostr_communikeys_coordinate_format(const nostr_communikeys_coordinate_t *coordinate);
void nostr_communikeys_coordinate_clear(nostr_communikeys_coordinate_t *coordinate);

/*
 * Section-scoped addressable identifier: "<communityId>-<purpose>[.<shard>]".
 * `purpose` matches ^[a-z0-9]+(-[a-z0-9]+)*$; the period is a reserved shard
 * separator and `shard` is a canonical decimal integer starting at 2. The
 * complete `d` is at most NOSTR_COMMUNIKEYS_MAX_SECTION_D UTF-8 bytes.
 * On success *out_purpose (caller frees) and *out_shard (1 when unsharded)
 * are set.
 */
bool nostr_communikeys_section_identifier_parse(const char *d,
                                                const char *community_id,
                                                char **out_purpose,
                                                int *out_shard);

typedef struct {
    int kind;
    char *subtype; /* NULL and the empty string both mean the exact empty subtype. */
} nostr_communikeys_assignment_t;

/* One `a` profile-list reference inside a section (zero or more per section). */
typedef struct {
    nostr_communikeys_coordinate_t coordinate; /* kind 30000, real signer author */
    char *purpose; /* parsed section-purpose token */
    int shard;     /* 1 when unsharded, >=2 for ".<shard>" suffixes */
} nostr_communikeys_profile_list_ref_t;

typedef struct {
    char *coordinate; /* "30009:<author>:<identifier>" */
    char *relay;      /* optional hint */
} nostr_communikeys_badge_ref_t;

typedef enum {
    NOSTR_COMMUNIKEYS_RETENTION_TIME,
    NOSTR_COMMUNIKEYS_RETENTION_COUNT
} nostr_communikeys_retention_type_t;

typedef struct {
    int kind;
    int64_t value; /* canonical positive decimal */
    nostr_communikeys_retention_type_t type;
} nostr_communikeys_retention_t;

typedef struct {
    char *name;
    nostr_communikeys_assignment_t *assignments;
    size_t assignments_len;
    nostr_communikeys_profile_list_ref_t *profile_lists;
    size_t profile_lists_len; /* MAY be zero: a grant-free section is valid */
    nostr_communikeys_badge_ref_t *badges;
    size_t badges_len;
    nostr_communikeys_retention_t *retention;
    size_t retention_len;
} nostr_communikeys_section_t;

typedef struct {
    char *url;
    char *protocol;
} nostr_communikeys_mint_t;

/* ["service", name, servicePubkey, requestRelay, handlerAddress, handlerRelay] */
typedef struct {
    char *name; /* ^[a-z0-9][a-z0-9-]{0,31}$ */
    char pubkey[65];
    char *request_relay;
    char *handler_address;
    char *handler_relay;
} nostr_communikeys_service_t;

typedef struct {
    nostr_communikeys_branch_t branch; /* owner = event author, id = d tag */
    char *name; /* REQUIRED, 1..100 UTF-8 bytes */
    char *description;
    char *picture;
    char *banner;
    char *website;
    char **relays;
    size_t relays_len;
    char **blossom_servers;
    size_t blossom_servers_len;
    char **grasp_servers;
    size_t grasp_servers_len;
    nostr_communikeys_mint_t *mints;
    size_t mints_len;
    char *tos;
    char *tos_relay;
    char *location;
    char *geohash;
    nostr_communikeys_service_t *services;
    size_t services_len;
    nostr_communikeys_section_t *sections;
    size_t sections_len;

    /* Defensive parsing always preserves at most one resolution result.
     * Invalid definitions remain inspectable but MUST NOT be used for access. */
    bool valid;
    nostr_communikeys_status_t validation_status;
} nostr_communikeys_definition_t;

void nostr_communikeys_definition_clear(nostr_communikeys_definition_t *definition);

/* Parses a kind-32222 event defensively. Returns false only for a wrong kind,
 * invalid arguments, or allocation failure. Inspect out->valid before use. */
bool nostr_communikeys_definition_parse(const NostrEvent *event,
                                        nostr_communikeys_definition_t *out);
nostr_communikeys_status_t
nostr_communikeys_definition_validate_event(const NostrEvent *event);

/* Strict writer: returns NULL unless the model satisfies all definition rules. */
NostrEvent *
nostr_communikeys_definition_to_event(const nostr_communikeys_definition_t *definition,
                                      int64_t created_at);

const nostr_communikeys_section_t *
nostr_communikeys_definition_find_section(const nostr_communikeys_definition_t *definition,
                                          int kind,
                                          const char *subtype);

/* One adjacent ["h", communityId] + ["a", "32222:<owner>:<id>", relay?] pair.
 * The address identifier always equals community_id. */
typedef struct {
    char community_id[65];
    nostr_communikeys_branch_t branch;
    char *relay; /* optional hint */
} nostr_communikeys_community_target_t;

typedef enum {
    NOSTR_COMMUNIKEYS_REFERENCE_EVENT,
    NOSTR_COMMUNIKEYS_REFERENCE_ADDRESS
} nostr_communikeys_reference_type_t;

/*
 * Kind-30222 targeting wrapper (closed grammar). The wrapper author is a
 * CURATOR and need NOT be the original publication's author. The source is
 * optional: without one, the original uses the wrapper `d` as its targeting
 * `h` and shares the wrapper author.
 */
typedef struct {
    char *identifier; /* kind-30222 d value */
    bool has_source;
    nostr_communikeys_reference_type_t reference_type; /* valid when has_source */
    char *reference;        /* event id or address coordinate */
    char *reference_relay;
    char *reference_author; /* optional e author HINT (never enforced against
                             * the curator); derived for a references */
    int original_kind;
    char *curator; /* kind-30222 author (wrapper signer) */
    nostr_communikeys_community_target_t *targets;
    size_t targets_len; /* 1..NOSTR_COMMUNIKEYS_MAX_TARGETS adjacent pairs */
} nostr_communikeys_targeted_publication_t;

void nostr_communikeys_targeted_publication_clear(
    nostr_communikeys_targeted_publication_t *publication);

nostr_communikeys_status_t
nostr_communikeys_targeted_publication_parse(
    const NostrEvent *event,
    nostr_communikeys_targeted_publication_t *out);

/* If original is non-NULL, also verifies the source reference resolves to it
 * (event id or address) and that its kind equals the wrapper `k`. Authors are
 * NOT compared for sourced wrappers (curator wrappers are valid). For a
 * sourceless wrapper the original must carry `h` equal to the wrapper `d` and
 * share the wrapper author. */
nostr_communikeys_status_t
nostr_communikeys_targeted_publication_validate(
    const NostrEvent *event,
    const NostrEvent *original);

NostrEvent *
nostr_communikeys_targeted_publication_to_event(
    const nostr_communikeys_targeted_publication_t *publication,
    int64_t created_at);

/* Community-exclusive kinds 9/11: exactly one ["h", communityId].
 * The extracted value is an OPAQUE community ID. It MUST NOT be used as an
 * event author, person `p` target, or profile lookup, and it does not by
 * itself identify a branch — branch selection requires an explicit definition
 * address or admitting-branch context. */
nostr_communikeys_status_t
nostr_communikeys_exclusive_validate(const NostrEvent *event, char community_id[65]);
bool nostr_communikeys_exclusive_add_h(NostrEvent *event, const char *community_id);

/*
 * Canonical pointer: NIP-19 naddr(kind 32222, owner, communityId, hints).
 * Pointer equality compares the branch only; relay hints are retrieval hints.
 * Emission keeps at most NOSTR_COMMUNIKEYS_MAX_RELAY_HINTS normalized wss://
 * hints in declared order with duplicates removed by first occurrence.
 */
typedef struct {
    nostr_communikeys_branch_t branch;
    char **relays;
    size_t relays_len;
} nostr_communikeys_pointer_t;

void nostr_communikeys_pointer_clear(nostr_communikeys_pointer_t *pointer);
nostr_communikeys_status_t
nostr_communikeys_pointer_parse(const char *naddr,
                                nostr_communikeys_pointer_t *out);
char *nostr_communikeys_pointer_format(const nostr_communikeys_pointer_t *pointer);

typedef struct {
    char *publisher;
    char *identifier;
    char **members;
    size_t members_len;
} nostr_communikeys_profile_list_t;

void nostr_communikeys_profile_list_clear(nostr_communikeys_profile_list_t *list);
nostr_communikeys_status_t
nostr_communikeys_profile_list_parse(const NostrEvent *event,
                                     const char *expected_publisher,
                                     const char *expected_identifier,
                                     nostr_communikeys_profile_list_t *out);
bool nostr_communikeys_profile_list_contains(
    const nostr_communikeys_profile_list_t *list,
    const char *pubkey);

/*
 * Resolves one exact section and evaluates the V2 grant model:
 * - the definition owner retains inherent authority;
 * - every non-owner pubkey referenced as a profile-list author anywhere in
 *   the definition holds the structural community-wide member/write role;
 * - otherwise the effective grant set is the UNION of current valid `p` tags
 *   across every coordinate the section references (multi-shard union).
 *
 * profile_list_events is the caller-supplied set of CURRENT events at the
 * coordinates the section references; for each referenced coordinate the
 * newest matching event (greatest created_at, ties broken by lexicographically
 * lowest id) contributes. A NULL/empty set means no p-tag grants, which is
 * valid (a grant-free section) rather than an error.
 */
bool nostr_communikeys_author_can_publish(
    const nostr_communikeys_definition_t *definition,
    int kind,
    const char *subtype,
    const NostrEvent *const *profile_list_events,
    size_t profile_list_events_len,
    const char *author_pubkey);

#ifdef __cplusplus
}
#endif

#endif
