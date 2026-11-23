#include "nostr/nip78/nip78.h"
#include "nostr-kinds.h"
#include "codec-internal.h"

NostrEvent *nostr_nip78_build_event(const char *author, const char *identifier,
                                   const char *content, int64_t created_at) {
    if (!identifier) { errno = EINVAL; return NULL; }
    NostrTags *tags = nostr_tags_new(0);
    if (!tags) { errno = ENOMEM; return NULL; }
    const char *d[] = {"d", identifier};
    int rc = codec_tag_append(tags, d, 2);
    if (rc) { nostr_tags_free(tags); errno = -rc; return NULL; }
    return codec_event(NOSTR_KIND_APPLICATION_SPECIFIC_DATA, author, created_at,
                       tags, content);
}

void nostr_nip78_document_free(NostrNip78Document *document) {
    if (!document) return;
    free(document->author);
    free(document->identifier);
    free(document->content);
    free(document->event_id);
    free(document);
}

int nostr_nip78_parse_event(const NostrEvent *event, NostrNip78Document **out) {
    if (!out) return -EINVAL;
    *out = NULL;
    if (!event || nostr_event_get_kind(event) != NOSTR_KIND_APPLICATION_SPECIFIC_DATA ||
        !codec_hex(nostr_event_get_pubkey(event), 64) ||
        nostr_event_get_created_at(event) < 0 || !nostr_event_get_content(event))
        return -EINVAL;
    const char *identifier = NULL;
    size_t count = 0;
    NostrTags *tags = nostr_event_get_tags(event);
    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        const char *key = codec_field(tag, 0);
        if (!key || strcmp(key, "d")) continue;
        if (++count != 1 || nostr_tag_size(tag) != 2 ||
            !(identifier = codec_field(tag, 1))) return -EINVAL;
    }
    if (!identifier) return -EINVAL;
    char id[65];
    if (nostr_event_compute_id(event, id) != NOSTR_EVENT_VALIDATION_OK) return -EINVAL;
    NostrNip78Document *document = calloc(1, sizeof(*document));
    if (!document) return -ENOMEM;
    document->author = strdup(nostr_event_get_pubkey(event));
    document->identifier = strdup(identifier);
    document->content = strdup(nostr_event_get_content(event));
    document->created_at = nostr_event_get_created_at(event);
    document->event_id = strdup(id);
    if (!document->author || !document->identifier || !document->content || !document->event_id) {
        nostr_nip78_document_free(document);
        return -ENOMEM;
    }
    *out = document;
    return 0;
}
