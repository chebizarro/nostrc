#include "nostr/nip51/nip51.h"
#include "nostr-kinds.h"
#include "../../codec-internal.h"

struct NostrListSchema { int kind; bool identifier, read_only; NostrListPrivacy privacy; const char *items; };
#define OPTIONAL NOSTR_LIST_PRIVACY_OPTIONAL
#define PUBLIC NOSTR_LIST_PRIVACY_PUBLIC
#define PRIVATE NOSTR_LIST_PRIVACY_PRIVATE
/* Protocol table pinned in tests/schema-registry.tsv. Definitions are not
 * executable commands; validation never interprets command content. */
static const NostrListSchema schemas[] = {
    {NOSTR_KIND_FOLLOW_LIST, false, true, PUBLIC, "p"},
    {NOSTR_KIND_RELAY_LIST_METADATA, false, true, PUBLIC, "r"},
    {NOSTR_KIND_MUTE_LIST, false, false, OPTIONAL, "p t word e"},
    {NOSTR_KIND_PIN_LIST, false, false, OPTIONAL, "e"},
    {NOSTR_KIND_BOOKMARK_LIST, false, false, OPTIONAL, "e a r t"},
    {NOSTR_KIND_BLOCKED_RELAY_LIST, false, false, OPTIONAL, "relay"},
    {NOSTR_KIND_PROFILE_BADGE_LIST, false, false, OPTIONAL, "a e"},
    {NOSTR_KIND_SIMPLE_GROUP_LIST, false, false, OPTIONAL, "group r"},
    {NOSTR_KIND_PRIVATE_RELAY_LIST, false, false, PRIVATE, "relay"},
    {NOSTR_KIND_EMOJI_LIST, false, false, OPTIONAL, "emoji a"},
    {NOSTR_KIND_DM_RELAY_LIST, false, false, PUBLIC, "relay"},
    {NOSTR_KIND_USER_SERVER_LIST, false, false, PUBLIC, "server"},
    {NOSTR_KIND_SLASH_COMMAND_LIST, false, false, OPTIONAL, "a"},
    {NOSTR_KIND_CATEGORIZED_PEOPLE_LIST, true, false, OPTIONAL, "p"},
    {NOSTR_KIND_CATEGORIZED_BOOKMARKS_LIST, true, false, OPTIONAL, "e a r t"},
    {NOSTR_KIND_RELAY_SETS, true, false, OPTIONAL, "relay"},
    {NOSTR_KIND_BOOKMARK_SETS, true, false, OPTIONAL, "e a r t"},
    {NOSTR_KIND_CURATED_SETS, true, false, OPTIONAL, "a e"},
    {NOSTR_KIND_CURATED_VIDEO_SETS, true, false, OPTIONAL, "e"},
    {NOSTR_KIND_CURATED_PICTURE_SETS, true, false, OPTIONAL, "e"},
    {NOSTR_KIND_MUTE_SETS, true, false, OPTIONAL, "p"},
    {NOSTR_KIND_BADGE_SETS, true, false, OPTIONAL, "a e"},
    {NOSTR_KIND_INTEREST_SETS, true, false, OPTIONAL, "t"},
    {NOSTR_KIND_EMOJI_SETS, true, false, OPTIONAL, "emoji"},
    {NOSTR_KIND_RELEASE_ARTIFACT_SETS, true, false, OPTIONAL, "e a"},
    {NOSTR_KIND_APP_CURATION_SETS, true, false, OPTIONAL, "a"},
    {NOSTR_KIND_CALENDAR, true, false, OPTIONAL, "a"},
    {NOSTR_KIND_STARTER_PACK, true, false, OPTIONAL, "p"},
    {NOSTR_KIND_MEDIA_STARTER_PACK, true, false, OPTIONAL, "p"},
    {NOSTR_KIND_SLASH_COMMAND_DEFINITION, true, false, OPTIONAL, "command title description arg s"},
};
size_t nostr_nip51_schema_count(void) { return sizeof(schemas) / sizeof(schemas[0]); }
const NostrListSchema *nostr_nip51_schema_at(size_t i) { return i < nostr_nip51_schema_count() ? &schemas[i] : NULL; }
const NostrListSchema *nostr_nip51_schema_lookup(int kind) {
    for (size_t i = 0; i < nostr_nip51_schema_count(); i++) if (schemas[i].kind == kind) return &schemas[i];
    return NULL;
}
int nostr_nip51_schema_kind(const NostrListSchema *s) { return s ? s->kind : -1; }
bool nostr_nip51_schema_requires_identifier(const NostrListSchema *s) { return s && s->identifier; }
bool nostr_nip51_schema_read_only(const NostrListSchema *s) { return s && s->read_only; }
NostrListPrivacy nostr_nip51_schema_privacy(const NostrListSchema *s) { return s ? s->privacy : PUBLIC; }
const char *nostr_nip51_schema_items(const NostrListSchema *s) { return s ? s->items : NULL; }
const char *nostr_nip51_schema_metadata(const NostrListSchema *s) {
    return s ? (s->identifier ? "d title image description" : "title image description") : NULL;
}
static bool known(const char *items, const char *key) {
    size_t n = strlen(key);
    if (!n) return false;
    for (const char *p = items; *p;) {
        const char *end = strchr(p, ' ');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == n && !memcmp(p, key, n)) return true;
        if (!end) break;
        p = end + 1;
    }
    return false;
}
NostrListItemValidation nostr_nip51_validate_item(const NostrListSchema *s, const NostrListTag *t) {
    if (!s || !t) return NOSTR_LIST_ITEM_INVALID;
    const char *k = nostr_nip51_tag_field(t, 0), *v = nostr_nip51_tag_field(t, 1);
    if (!k) return NOSTR_LIST_ITEM_EXTENSION;
    if (!strcmp(k, "d")) return v && strlen(v) <= NOSTR_LIST_MAX_IDENTIFIER_BYTES ? NOSTR_LIST_ITEM_VALID : NOSTR_LIST_ITEM_INVALID;
    if (!strcmp(k, "title") || !strcmp(k, "image") || !strcmp(k, "description"))
        return v ? NOSTR_LIST_ITEM_VALID : NOSTR_LIST_ITEM_INVALID;
    if (!known(s->items, k)) return NOSTR_LIST_ITEM_EXTENSION;
    if (!v || !*v) return NOSTR_LIST_ITEM_INVALID;
    bool valid = true;
    if (!strcmp(k, "p") || !strcmp(k, "e")) valid = codec_hex(v, 64);
    else if (!strcmp(k, "relay")) valid = codec_url(v, true);
    else if (!strcmp(k, "server")) valid = codec_url(v, false);
    else if (!strcmp(k, "r") && (s->kind == NOSTR_KIND_RELAY_LIST_METADATA || s->kind == NOSTR_KIND_SIMPLE_GROUP_LIST)) {
        valid = codec_url(v, true);
        const char *marker = nostr_nip51_tag_field(t, 2);
        if (s->kind == NOSTR_KIND_RELAY_LIST_METADATA && marker && *marker && strcmp(marker, "read") && strcmp(marker, "write")) valid = false;
    } else if (!strcmp(k, "group")) valid = codec_url(nostr_nip51_tag_field(t, 2), true);
    else if (!strcmp(k, "emoji")) {
        for (const char *p = v; *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) valid = false;
        valid = valid && codec_url(nostr_nip51_tag_field(t, 2), false);
        const char *set = nostr_nip51_tag_field(t, 3);
        int kind;
        if (set && (!codec_address(set, &kind, NULL, NULL) || kind != NOSTR_KIND_EMOJI_SETS)) valid = false;
    } else if (!strcmp(k, "a")) {
        int kind;
        valid = codec_address(v, &kind, NULL, NULL);
        if (valid) {
            switch (s->kind) {
            case NOSTR_KIND_PROFILE_BADGE_LIST: valid = kind == NOSTR_KIND_BADGE_DEFINITION || kind == NOSTR_KIND_BADGE_SETS; break;
            case NOSTR_KIND_BADGE_SETS: valid = kind == NOSTR_KIND_BADGE_DEFINITION; break;
            case NOSTR_KIND_EMOJI_LIST: valid = kind == NOSTR_KIND_EMOJI_SETS; break;
            case NOSTR_KIND_SLASH_COMMAND_LIST: valid = kind == NOSTR_KIND_SLASH_COMMAND_DEFINITION; break;
            case NOSTR_KIND_CURATED_SETS: valid = kind == NOSTR_KIND_ARTICLE; break;
            case NOSTR_KIND_CALENDAR: valid = kind == NOSTR_KIND_DATE_CALENDAR_EVENT || kind == NOSTR_KIND_TIME_CALENDAR_EVENT; break;
            default: break;
            }
        }
    } else if (!strcmp(k, "arg")) {
        const char *type = nostr_nip51_tag_field(t, 2), *required = nostr_nip51_tag_field(t, 3);
        valid = type && *type && required && (!strcmp(required, "required") || !strcmp(required, "optional")) && nostr_nip51_tag_field(t, 4);
    }
    return valid ? NOSTR_LIST_ITEM_VALID : NOSTR_LIST_ITEM_INVALID;
}
