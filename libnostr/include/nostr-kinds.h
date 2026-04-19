#ifndef __NOSTR_KINDS_H__
#define __NOSTR_KINDS_H__

/* NIP-01: Basic protocol */
#define NOSTR_KIND_PROFILE_METADATA             0
#define NOSTR_KIND_TEXT_NOTE                     1
#define NOSTR_KIND_RECOMMEND_SERVER              2
#define NOSTR_KIND_FOLLOW_LIST                   3
#define NOSTR_KIND_ENCRYPTED_DIRECT_MESSAGE      4
#define NOSTR_KIND_DELETION                      5
#define NOSTR_KIND_REPOST                        6
#define NOSTR_KIND_REACTION                      7
#define NOSTR_KIND_BADGE_AWARD                   8
#define NOSTR_KIND_SIMPLE_GROUP_CHAT_MESSAGE     9
#define NOSTR_KIND_SIMPLE_GROUP_THREADED_REPLY   10
#define NOSTR_KIND_SIMPLE_GROUP_THREAD           11
#define NOSTR_KIND_SIMPLE_GROUP_REPLY            12
#define NOSTR_KIND_SEAL                          13
#define NOSTR_KIND_DIRECT_MESSAGE                14
#define NOSTR_KIND_FILE_MESSAGE                  15
#define NOSTR_KIND_GENERIC_REPOST                16
#define NOSTR_KIND_REACTION_TO_WEBSITE           17
#define NOSTR_KIND_PHOTO                         20
#define NOSTR_KIND_NORMAL_VIDEO                  21
#define NOSTR_KIND_SHORT_VIDEO                   22

/* NIP-28: Public chat */
#define NOSTR_KIND_CHANNEL_CREATION              40
#define NOSTR_KIND_CHANNEL_METADATA              41
#define NOSTR_KIND_CHANNEL_MESSAGE               42
#define NOSTR_KIND_CHANNEL_HIDE_MESSAGE          43
#define NOSTR_KIND_CHANNEL_MUTE_USER             44

/* NIP-64: Chess */
#define NOSTR_KIND_CHESS                         64

/* NIP-34: Git */
#define NOSTR_KIND_MERGE_REQUESTS                818

/* Polls */
#define NOSTR_KIND_POLL_RESPONSE                 1018
#define NOSTR_KIND_BID                           1021
#define NOSTR_KIND_BID_CONFIRMATION              1022
#define NOSTR_KIND_OPEN_TIMESTAMPS               1040
#define NOSTR_KIND_GIFT_WRAP                     1059
#define NOSTR_KIND_FILE_METADATA                 1063
#define NOSTR_KIND_POLL                          1068

/* NIP-22: Comments */
#define NOSTR_KIND_COMMENT                       1111

/* Voice */
#define NOSTR_KIND_VOICE                         1222
#define NOSTR_KIND_VOICE_COMMENT                 1244

#define NOSTR_KIND_LIVE_CHAT_MESSAGE             1311

/* NIP-34: Git patches/issues */
#define NOSTR_KIND_PATCH                         1617
#define NOSTR_KIND_ISSUE                         1621
#define NOSTR_KIND_REPLY                         1622
#define NOSTR_KIND_STATUS_OPEN                   1630
#define NOSTR_KIND_STATUS_APPLIED                1631
#define NOSTR_KIND_STATUS_CLOSED                 1632
#define NOSTR_KIND_STATUS_DRAFT                  1633

#define NOSTR_KIND_PROBLEM_TRACKER               1971
#define NOSTR_KIND_REPORTING                     1984
#define NOSTR_KIND_LABEL                         1985
#define NOSTR_KIND_RELAY_REVIEWS                 1986
#define NOSTR_KIND_AI_EMBEDDINGS                 1987
#define NOSTR_KIND_TORRENT                       2003
#define NOSTR_KIND_TORRENT_COMMENT               2004
#define NOSTR_KIND_COINJOIN_POOL                 2022
#define NOSTR_KIND_COMMUNITY_POST_APPROVAL       4550

/* NIP-90: Data vending machine */
#define NOSTR_KIND_JOB_REQUEST                   5999
#define NOSTR_KIND_JOB_RESULT                    6999
#define NOSTR_KIND_JOB_FEEDBACK                  7000

/* NIP-29: Relay-based groups (moderation) */
#define NOSTR_KIND_SIMPLE_GROUP_ADD_USER         9000
#define NOSTR_KIND_SIMPLE_GROUP_REMOVE_USER      9001
#define NOSTR_KIND_SIMPLE_GROUP_EDIT_METADATA    9002
#define NOSTR_KIND_SIMPLE_GROUP_ADD_PERMISSION   9003
#define NOSTR_KIND_SIMPLE_GROUP_REMOVE_PERMISSION 9004
#define NOSTR_KIND_SIMPLE_GROUP_DELETE_EVENT     9005
#define NOSTR_KIND_SIMPLE_GROUP_EDIT_GROUP_STATUS 9006
#define NOSTR_KIND_SIMPLE_GROUP_CREATE_GROUP     9007
#define NOSTR_KIND_SIMPLE_GROUP_DELETE_GROUP     9008
#define NOSTR_KIND_SIMPLE_GROUP_CREATE_INVITE    9009
#define NOSTR_KIND_SIMPLE_GROUP_JOIN_REQUEST     9021
#define NOSTR_KIND_SIMPLE_GROUP_LEAVE_REQUEST    9022

/* NIP-75: Zap Goals */
#define NOSTR_KIND_ZAP_GOAL                      9041

/* NIP-61: Nutzaps */
#define NOSTR_KIND_NUTZAP                        9321

#define NOSTR_KIND_TIDAL_LOGIN                   9467

/* NIP-57: Lightning Zaps */
#define NOSTR_KIND_ZAP_REQUEST                   9734
#define NOSTR_KIND_ZAP                           9735

#define NOSTR_KIND_HIGHLIGHTS                    9802

/* Replaceable events (10000-19999) */
#define NOSTR_KIND_MUTE_LIST                     10000
#define NOSTR_KIND_PIN_LIST                      10001
#define NOSTR_KIND_RELAY_LIST_METADATA           10002
#define NOSTR_KIND_BOOKMARK_LIST                 10003
#define NOSTR_KIND_COMMUNITY_LIST                10004
#define NOSTR_KIND_PUBLIC_CHAT_LIST              10005
#define NOSTR_KIND_BLOCKED_RELAY_LIST            10006
#define NOSTR_KIND_SEARCH_RELAY_LIST             10007
#define NOSTR_KIND_SIMPLE_GROUP_LIST             10009
#define NOSTR_KIND_FAVORITE_RELAYS               10012
#define NOSTR_KIND_INTEREST_LIST                 10015
#define NOSTR_KIND_NUTZAP_INFO                   10019
#define NOSTR_KIND_EMOJI_LIST                    10030
#define NOSTR_KIND_DM_RELAY_LIST                 10050
#define NOSTR_KIND_USER_SERVER_LIST              10063
#define NOSTR_KIND_BLOSSOM_SERVER_LIST           10063  /* alias */
#define NOSTR_KIND_FILE_STORAGE_SERVER_LIST      10096
#define NOSTR_KIND_GOOD_WIKI_AUTHOR_LIST         10101
#define NOSTR_KIND_GOOD_WIKI_RELAY_LIST          10102

/* NIP-47: Wallet Connect */
#define NOSTR_KIND_NWC_WALLET_INFO               13194

/* Ephemeral events (20000-29999) */
#define NOSTR_KIND_LIGHTNING_PUB_RPC             21000
#define NOSTR_KIND_CLIENT_AUTHENTICATION         22242
#define NOSTR_KIND_NWC_WALLET_REQUEST            23194
#define NOSTR_KIND_NWC_WALLET_RESPONSE           23195
#define NOSTR_KIND_NOSTR_CONNECT                 24133
#define NOSTR_KIND_BLOBS                         24242
#define NOSTR_KIND_HTTP_AUTH                      27235

/* Addressable events (30000-39999) */
#define NOSTR_KIND_CATEGORIZED_PEOPLE_LIST       30000
#define NOSTR_KIND_CATEGORIZED_BOOKMARKS_LIST    30001
#define NOSTR_KIND_RELAY_SETS                    30002
#define NOSTR_KIND_BOOKMARK_SETS                 30003
#define NOSTR_KIND_CURATED_SETS                  30004
#define NOSTR_KIND_CURATED_VIDEO_SETS            30005
#define NOSTR_KIND_MUTE_SETS                     30007
#define NOSTR_KIND_PROFILE_BADGES                30008
#define NOSTR_KIND_BADGE_DEFINITION              30009
#define NOSTR_KIND_INTEREST_SETS                 30015
#define NOSTR_KIND_STALL_DEFINITION              30017
#define NOSTR_KIND_PRODUCT_DEFINITION            30018
#define NOSTR_KIND_MARKETPLACE_UI                30019
#define NOSTR_KIND_PRODUCT_SOLD_AS_AUCTION       30020
#define NOSTR_KIND_ARTICLE                       30023
#define NOSTR_KIND_DRAFT_ARTICLE                 30024
#define NOSTR_KIND_EMOJI_SETS                    30030
#define NOSTR_KIND_MODULAR_ARTICLE_HEADER        30040
#define NOSTR_KIND_MODULAR_ARTICLE_CONTENT       30041
#define NOSTR_KIND_RELEASE_ARTIFACT_SETS         30063
#define NOSTR_KIND_APPLICATION_SPECIFIC_DATA     30078
#define NOSTR_KIND_LIVE_EVENT                    30311
#define NOSTR_KIND_USER_STATUSES                 30315
#define NOSTR_KIND_CLASSIFIED_LISTING            30402
#define NOSTR_KIND_DRAFT_CLASSIFIED_LISTING      30403
#define NOSTR_KIND_REPOSITORY_ANNOUNCEMENT       30617
#define NOSTR_KIND_REPOSITORY_STATE              30618
#define NOSTR_KIND_WIKI_ARTICLE                  30818
#define NOSTR_KIND_REDIRECTS                     30819
#define NOSTR_KIND_FEED                          31890
#define NOSTR_KIND_DATE_CALENDAR_EVENT           31922
#define NOSTR_KIND_TIME_CALENDAR_EVENT           31923
#define NOSTR_KIND_CALENDAR                      31924
#define NOSTR_KIND_CALENDAR_EVENT_RSVP           31925
#define NOSTR_KIND_RELAY_REVIEW                  31987
#define NOSTR_KIND_HANDLER_RECOMMENDATION        31989
#define NOSTR_KIND_HANDLER_INFORMATION           31990
#define NOSTR_KIND_VIDEO_EVENT                   34235
#define NOSTR_KIND_SHORT_VIDEO_EVENT             34236
#define NOSTR_KIND_VIDEO_VIEW_EVENT              34237
#define NOSTR_KIND_COMMUNITY_DEFINITION          34550

/* NIP-29: Relay-based groups (metadata, addressable) */
#define NOSTR_KIND_SIMPLE_GROUP_METADATA         39000
#define NOSTR_KIND_SIMPLE_GROUP_ADMINS           39001
#define NOSTR_KIND_SIMPLE_GROUP_MEMBERS          39002
#define NOSTR_KIND_SIMPLE_GROUP_ROLES            39003
#define NOSTR_KIND_SIMPLE_GROUP_LIVEKIT_PARTICIPANTS 39004

/* ============== Kind Classification ============== */

#include <stdbool.h>

/**
 * NostrKindClass:
 * @NOSTR_KIND_CLASS_REGULAR: Regular event (stored by relays)
 * @NOSTR_KIND_CLASS_REPLACEABLE: Only latest per pubkey+kind kept
 * @NOSTR_KIND_CLASS_EPHEMERAL: Not expected to be stored
 * @NOSTR_KIND_CLASS_ADDRESSABLE: Only latest per pubkey+kind+d-tag kept
 * @NOSTR_KIND_CLASS_UNKNOWN: Kind >= 40000, not classified
 *
 * Classification of an event kind per NIP-01.
 */
typedef enum {
  NOSTR_KIND_CLASS_REGULAR,
  NOSTR_KIND_CLASS_REPLACEABLE,
  NOSTR_KIND_CLASS_EPHEMERAL,
  NOSTR_KIND_CLASS_ADDRESSABLE,
  NOSTR_KIND_CLASS_UNKNOWN
} NostrKindClass;

/**
 * nostr_kind_is_regular:
 * @kind: event kind number
 *
 * Regular events are expected to be stored by relays.
 * Kind 0 (metadata) and 3 (contacts) are replaceable, not regular.
 *
 * Returns: true if the kind is a regular event kind
 */
static inline bool nostr_kind_is_regular(int kind) {
  return kind < 10000 && kind != 0 && kind != 3;
}

/**
 * nostr_kind_is_replaceable:
 * @kind: event kind number
 *
 * Replaceable events: only the latest event per pubkey+kind is kept.
 * Includes kind 0, kind 3, and kinds 10000-19999.
 *
 * Returns: true if the kind is replaceable
 */
static inline bool nostr_kind_is_replaceable(int kind) {
  return kind == 0 || kind == 3 || (10000 <= kind && kind < 20000);
}

/**
 * nostr_kind_is_ephemeral:
 * @kind: event kind number
 *
 * Ephemeral events are not expected to be stored by relays.
 * Kinds 20000-29999.
 *
 * Returns: true if the kind is ephemeral
 */
static inline bool nostr_kind_is_ephemeral(int kind) {
  return 20000 <= kind && kind < 30000;
}

/**
 * nostr_kind_is_addressable:
 * @kind: event kind number
 *
 * Addressable (parameterized replaceable) events: only the latest event
 * per pubkey+kind+d-tag is kept. Kinds 30000-39999.
 *
 * Returns: true if the kind is addressable
 */
static inline bool nostr_kind_is_addressable(int kind) {
  return 30000 <= kind && kind < 40000;
}

/**
 * nostr_kind_classify:
 * @kind: event kind number
 *
 * Classify an event kind per NIP-01 rules.
 *
 * Returns: the kind classification
 */
static inline NostrKindClass nostr_kind_classify(int kind) {
  if (nostr_kind_is_replaceable(kind))  return NOSTR_KIND_CLASS_REPLACEABLE;
  if (nostr_kind_is_regular(kind))      return NOSTR_KIND_CLASS_REGULAR;
  if (nostr_kind_is_ephemeral(kind))    return NOSTR_KIND_CLASS_EPHEMERAL;
  if (nostr_kind_is_addressable(kind))  return NOSTR_KIND_CLASS_ADDRESSABLE;
  return NOSTR_KIND_CLASS_UNKNOWN;
}

#endif /* __NOSTR_KINDS_H__ */
