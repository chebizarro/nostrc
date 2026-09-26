/*
 * nsp-query — classify what the user typed into the overview and build
 * the NIP-01 REQ filters for the session relay.
 *
 *   input                               class     primary filter(s)
 *   ----------------------------------  --------  ----------------------------------------------
 *   npub1… / nprofile1… (± nostr:)      PROFILE   {"kinds":[0],"authors":[pk],"limit":1}
 *   note1… / nevent1…                   EVENT     {"ids":[id],"limit":1}
 *   naddr1…                             ADDRESS   {"kinds":[k],"authors":[pk],"#d":[d],"limit":1}
 *   64 hex chars                        HEX       {"ids":[h],"limit":1},{"kinds":[0],"authors":[h],"limit":1}
 *   local@domain.tld                    NIP05     resolved pk → PROFILE filter; plus
 *                                                 {"kinds":[0],"search":"local@domain.tld","limit":20}
 *   anything else (≥ 3 code points)     TEXT      {"kinds":[0,1,30023],"search":"<text>","limit":40}
 *                                                 on CLOSED "unsupported: search" (no NIP-50):
 *                                                 {"kinds":[0],"limit":250},{"kinds":[1,30023],"limit":250}
 *                                                 then match every word client-side
 *   nsec1… / ncryptsec1… / partial      NONE      nothing (secrets are never sent anywhere)
 *
 * Follow-up: note results without a known author get one
 * {"kinds":[0],"authors":[…],"limit":n} REQ if the deadline allows.
 */
#ifndef NSP_QUERY_H
#define NSP_QUERY_H

#include <glib.h>
#include <json-glib/json-glib.h>

#include "nd-uri.h"

G_BEGIN_DECLS

#define NSP_TEXT_MIN_CHARS 3
#define NSP_TEXT_MAX_BYTES 256
#define NSP_TEXT_MAX_WORDS 8
#define NSP_TEXT_SEARCH_LIMIT 40
#define NSP_NIP05_CLAIM_LIMIT 20
#define NSP_SCAN_PROFILE_LIMIT 250
#define NSP_SCAN_NOTE_LIMIT 250

typedef enum {
  NSP_QUERY_NONE = 0,
  NSP_QUERY_PROFILE,
  NSP_QUERY_EVENT,
  NSP_QUERY_ADDRESS,
  NSP_QUERY_HEX,
  NSP_QUERY_NIP05,
  NSP_QUERY_TEXT,
} NspQueryType;

typedef struct {
  NspQueryType type;
  NdTarget *target;   /* PROFILE / EVENT / ADDRESS */
  char *hex;          /* HEX (lowercase) */
  char *nip05;        /* NIP05: "local@domain", lower-cased */
  char *nip05_local;  /* NIP05: lower-cased local part */
  char *nip05_domain; /* NIP05: lower-cased domain */
  char *text;         /* TEXT: sanitized, single-spaced, <= NSP_TEXT_MAX_BYTES */
  char **words;       /* TEXT (and NIP05, for narrowing): folded words */
} NspQuery;

NspQuery *nsp_query_classify(const char *const *terms);
void nsp_query_free(NspQuery *q);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(NspQuery, nsp_query_free)

const char *nsp_query_type_name(NspQueryType t);

/* TRUE for an identifier lookup (exact, cheap): PROFILE/EVENT/ADDRESS/HEX/NIP05. */
gboolean nsp_query_is_identifier(const NspQuery *q);

/* Plausible, publicly resolvable NIP-05 domain (has a dot, alphabetic
 * TLD, not an IP literal, not .local/.localhost/.internal/.lan/
 * .home.arpa/.onion). Exposed for tests. */
gboolean nsp_nip05_domain_plausible(const char *domain);

/* Filter arrays (JSON array node of filter objects), see table above. */
JsonNode *nsp_query_filters(const NspQuery *q);
JsonNode *nsp_filters_profiles(const char *const *pubkeys);
JsonNode *nsp_filters_nip05_claim(const char *address);
JsonNode *nsp_filters_text_scan(void);

char *nsp_json_to_string(JsonNode *node);

G_END_DECLS

#endif /* NSP_QUERY_H */
