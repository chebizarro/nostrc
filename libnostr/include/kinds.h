#ifndef NOSTR_KINDS_H
#define NOSTR_KINDS_H

#define KIND_PROFILE_METADATA             0
#define KIND_TEXT_NOTE                    1
#define KIND_RECOMMEND_SERVER             2
#define KIND_FOLLOW_LIST                  3
#define KIND_ENCRYPTED_DIRECT_MESSAGE     4
#define KIND_DELETION                     5
#define KIND_REPOST                       6
#define KIND_REACTION                     7
#define KIND_BADGE_AWARD                  8
#define KIND_SIMPLE_GROUP_CHAT_MESSAGE    9
#define KIND_SIMPLE_GROUP_THREADED_REPLY  10
#define KIND_SIMPLE_GROUP_THREAD          11
#define KIND_SIMPLE_GROUP_REPLY           12
#define KIND_SEAL                         13
#define KIND_DIRECT_MESSAGE               14
#define KIND_GENERIC_REPOST               16
#define KIND_REACTION_TO_WEBSITE          17
#define KIND_CHANNEL_CREATION             40
#define KIND_CHANNEL_METADATA             41
#define KIND_CHANNEL_MESSAGE              42
#define KIND_CHANNEL_HIDE_MESSAGE         43
#define KIND_CHANNEL_MUTE_USER            44
#define KIND_CHESS                        64
#define KIND_MERGE_REQUESTS               818
#define KIND_BID                          1021
#define KIND_BID_CONFIRMATION             1022
#define KIND_OPEN_TIMESTAMPS              1040
#define KIND_GIFT_WRAP                    1059
#define KIND_FILE_METADATA                1063
#define KIND_LIVE_CHAT_MESSAGE            1311
#define KIND_PATCH                        1617
#define KIND_ISSUE                        1621
#define KIND_REPLY                        1622
#define KIND_STATUS_OPEN                  1630
#define KIND_STATUS_APPLIED               1631
#define KIND_STATUS_CLOSED                1632
#define KIND_STATUS_DRAFT                 1633
#define KIND_PROBLEM_TRACKER              1971
#define KIND_REPORTING                    1984
#define KIND_LABEL                        1985
#define KIND_RELAY_REVIEWS                1986
#define KIND_AI_EMBEDDINGS                1987
#define KIND_TORRENT                      2003
#define KIND_TORRENT_COMMENT              2004
#define KIND_COINJOIN_POOL                2022
#define KIND_COMMUNITY_POST_APPROVAL      4550
#define KIND_JOB_FEEDBACK                 7000
#define KIND_SIMPLE_GROUP_ADD_USER        9000
#define KIND_SIMPLE_GROUP_REMOVE_USER     9001
#define KIND_SIMPLE_GROUP_EDIT_METADATA   9002
#define KIND_SIMPLE_GROUP_ADD_PERMISSION  9003
#define KIND_SIMPLE_GROUP_REMOVE_PERMISSION 9004
#define KIND_SIMPLE_GROUP_DELETE_EVENT    9005
#define KIND_SIMPLE_GROUP_EDIT_GROUP_STATUS 9006
#define KIND_SIMPLE_GROUP_CREATE_GROUP    9007
#define KIND_SIMPLE_GROUP_DELETE_GROUP    9008
#define KIND_SIMPLE_GROUP_JOIN_REQUEST    9021
#define KIND_SIMPLE_GROUP_LEAVE_REQUEST   9022
#define KIND_ZAP_GOAL                     9041
#define KIND_TIDAL_LOGIN                  9467
#define KIND_ZAP_REQUEST                  9734
#define KIND_ZAP                          9735
#define KIND_HIGHLIGHTS                   9802
#define KIND_MUTE_LIST                    10000
#define KIND_PIN_LIST                     10001
#define KIND_RELAY_LIST_METADATA          10002
#define KIND_BOOKMARK_LIST                10003
#define KIND_COMMUNITY_LIST               10004
#define KIND_PUBLIC_CHAT_LIST             10005
#define KIND_BLOCKED_RELAY_LIST           10006
#define KIND_SEARCH_RELAY_LIST            10007
#define KIND_SIMPLE_GROUP_LIST            10009
#define KIND_INTEREST_LIST                10015
#define KIND_EMOJI_LIST                   10030
#define KIND_DM_RELAY_LIST                10050
#define KIND_USER_SERVER_LIST             10063
#define KIND_FILE_STORAGE_SERVER_LIST     10096
#define KIND_GOOD_WIKI_AUTHOR_LIST        10101
#define KIND_GOOD_WIKI_RELAY_LIST         10102
#define KIND_NWC_WALLET_INFO              13194
#define KIND_LIGHTNING_PUB_RPC            21000
#define KIND_CLIENT_AUTHENTICATION        22242
#define KIND_NWC_WALLET_REQUEST           23194
#define KIND_NWC_WALLET_RESPONSE          23195
#define KIND_NOSTR_CONNECT                24133
#define KIND_BLOBS                        24242
#define KIND_HTTP_AUTH                    27235
#define KIND_CATEGORIZED_PEOPLE_LIST      30000
#define KIND_CATEGORIZED_BOOKMARKS_LIST   30001
#define KIND_RELAY_SETS                   30002
#define KIND_BOOKMARK_SETS                30003
#define KIND_CURATED_SETS                 30004
#define KIND_CURATED_VIDEO_SETS           30005
#define KIND_MUTE_SETS                    30007
#define KIND_PROFILE_BADGES               30008
#define KIND_BADGE_DEFINITION             30009
#define KIND_INTEREST_SETS                30015
#define KIND_STALL_DEFINITION             30017
#define KIND_PRODUCT_DEFINITION           30018
#define KIND_MARKETPLACE_UI               30019
#define KIND_PRODUCT_SOLD_AS_AUCTION      30020
#define KIND_ARTICLE                      30023
#define KIND_DRAFT_ARTICLE                30024
#define KIND_EMOJI_SETS                   30030
#define KIND_MODULAR_ARTICLE_HEADER       30040
#define KIND_MODULAR_ARTICLE_CONTENT      30041
#define KIND_RELEASE_ARTIFACT_SETS        30063
#define KIND_APPLICATION_SPECIFIC_DATA    30078
#define KIND_LIVE_EVENT                   30311
#define KIND_USER_STATUSES                30315
#define KIND_CLASSIFIED_LISTING           30402
#define KIND_DRAFT_CLASSIFIED_LISTING     30403
#define KIND_REPOSITORY_ANNOUNCEMENT      30617
#define KIND_REPOSITORY_STATE             30618
#define KIND_SIMPLE_GROUP_METADATA        39000
#define KIND_WIKI_ARTICLE                 30818
#define KIND_REDIRECTS                    30819
#define KIND_FEED                         31890
#define KIND_DATE_CALENDAR_EVENT          31922
#define KIND_TIME_CALENDAR_EVENT          31923
#define KIND_CALENDAR                     31924
#define KIND_CALENDAR_EVENT_RSVP          31925
#define KIND_HANDLER_RECOMMENDATION       31989
#define KIND_HANDLER_INFORMATION          31990
#define KIND_VIDEO_EVENT                  34235
#define KIND_SHORT_VIDEO_EVENT            34236
#define KIND_VIDEO_VIEW_EVENT             34237
#define KIND_COMMUNITY_DEFINITION         34550
#define KIND_SIMPLE_GROUP_ADMINS          39001
#define KIND_SIMPLE_GROUP_MEMBERS         39002

#endif // NOSTR_KINDS_H