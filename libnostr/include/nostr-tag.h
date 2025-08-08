#ifndef __NOSTR_TAG_H__
#define __NOSTR_TAG_H__

/* Transitional header: standardized names for Tags/Tag APIs. */

#include <stddef.h>
#include <stdbool.h>
#include "tag.h" /* defines Tag, Tags and old function names */

#ifdef __cplusplus
extern "C" {
#endif

/* Canonical type names */
typedef Tag  NostrTag;
typedef Tags NostrTags;

/* New API mapped to legacy */
#define nostr_tag_new                 create_tag
#define nostr_tag_free                free_tag
#define nostr_tag_starts_with         tag_starts_with
#define nostr_tag_get_key             tag_key
#define nostr_tag_get_value           tag_value
#define nostr_tag_get_relay           tag_relay
#define nostr_tag_to_json             tag_marshal_to_json

#define nostr_tags_new                create_tags
#define nostr_tags_free               free_tags
#define nostr_tags_get_d              tags_get_d
#define nostr_tags_get_first          tags_get_first
#define nostr_tags_get_last           tags_get_last
#define nostr_tags_get_all            tags_get_all
#define nostr_tags_filter_out         tags_filter_out
#define nostr_tags_append_unique      tags_append_unique
#define nostr_tags_contains_any       tags_contains_any
#define nostr_tags_to_json            tags_marshal_to_json

/* Legacy aliases (temporary) */
#ifdef NOSTR_ENABLE_LEGACY_ALIASES
#  define create_tag                  nostr_tag_new
#  define free_tag                    nostr_tag_free
#  define tag_starts_with             nostr_tag_starts_with
#  define tag_key                     nostr_tag_get_key
#  define tag_value                   nostr_tag_get_value
#  define tag_relay                   nostr_tag_get_relay
#  define tag_marshal_to_json         nostr_tag_to_json
#  define create_tags                 nostr_tags_new
#  define free_tags                   nostr_tags_free
#  define tags_get_d                  nostr_tags_get_d
#  define tags_get_first              nostr_tags_get_first
#  define tags_get_last               nostr_tags_get_last
#  define tags_get_all                nostr_tags_get_all
#  define tags_filter_out             nostr_tags_filter_out
#  define tags_append_unique          nostr_tags_append_unique
#  define tags_contains_any           nostr_tags_contains_any
#  define tags_marshal_to_json        nostr_tags_to_json
#endif

#ifdef __cplusplus
}
#endif

#endif /* __NOSTR_TAG_H__ */
