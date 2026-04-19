#include "nostr/nip39/nip39.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Platform string table */
static const struct {
    const char *name;
    NostrNip39Platform platform;
} platform_table[] = {
    { "github",   NOSTR_NIP39_PLATFORM_GITHUB   },
    { "twitter",  NOSTR_NIP39_PLATFORM_TWITTER  },
    { "mastodon", NOSTR_NIP39_PLATFORM_MASTODON },
    { "telegram", NOSTR_NIP39_PLATFORM_TELEGRAM },
    { "keybase",  NOSTR_NIP39_PLATFORM_KEYBASE  },
    { "dns",      NOSTR_NIP39_PLATFORM_DNS      },
    { "reddit",   NOSTR_NIP39_PLATFORM_REDDIT   },
    { "website",  NOSTR_NIP39_PLATFORM_WEBSITE  },
    { NULL,       NOSTR_NIP39_PLATFORM_UNKNOWN  }
};

NostrNip39Platform nostr_nip39_platform_from_string(const char *str) {
    if (!str) return NOSTR_NIP39_PLATFORM_UNKNOWN;

    for (int i = 0; platform_table[i].name != NULL; ++i) {
        if (strcmp(str, platform_table[i].name) == 0)
            return platform_table[i].platform;
    }
    return NOSTR_NIP39_PLATFORM_UNKNOWN;
}

const char *nostr_nip39_platform_to_string(NostrNip39Platform platform) {
    for (int i = 0; platform_table[i].name != NULL; ++i) {
        if (platform_table[i].platform == platform)
            return platform_table[i].name;
    }
    return "unknown";
}

NostrNip39Platform nostr_nip39_detect_platform(const char *value) {
    if (!value) return NOSTR_NIP39_PLATFORM_UNKNOWN;

    const char *colon = strchr(value, ':');
    if (!colon || colon == value) return NOSTR_NIP39_PLATFORM_UNKNOWN;

    size_t prefix_len = (size_t)(colon - value);

    for (int i = 0; platform_table[i].name != NULL; ++i) {
        if (strlen(platform_table[i].name) == prefix_len &&
            memcmp(value, platform_table[i].name, prefix_len) == 0)
            return platform_table[i].platform;
    }
    return NOSTR_NIP39_PLATFORM_UNKNOWN;
}

/*
 * Parse a single "i" tag into an identity entry.
 */
static bool parse_identity(NostrTag *t, NostrNip39Identity *entry) {
    const char *v = nostr_tag_get(t, 1);
    if (!v || *v == '\0') return false;

    /* Must have a colon separating platform:identity */
    const char *colon = strchr(v, ':');
    if (!colon || colon == v || *(colon + 1) == '\0')
        return false;

    entry->value = v;
    entry->identity = colon + 1;
    entry->platform = nostr_nip39_detect_platform(v);

    /* Optional proof URL in third element */
    if (nostr_tag_size(t) >= 3) {
        const char *proof = nostr_tag_get(t, 2);
        entry->proof_url = (proof && *proof != '\0') ? proof : NULL;
    } else {
        entry->proof_url = NULL;
    }

    return true;
}

int nostr_nip39_parse(const NostrEvent *ev, NostrNip39Identity *entries,
                       size_t max_entries, size_t *out_count) {
    if (!ev || !entries || !out_count) return -EINVAL;
    *out_count = 0;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return 0;

    size_t n = nostr_tags_size(tags);
    size_t count = 0;

    for (size_t i = 0; i < n && count < max_entries; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;

        const char *k = nostr_tag_get(t, 0);
        if (!k || strcmp(k, "i") != 0) continue;

        if (parse_identity(t, &entries[count]))
            ++count;
    }

    *out_count = count;
    return 0;
}

size_t nostr_nip39_count(const NostrEvent *ev) {
    if (!ev) return 0;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return 0;

    size_t n = nostr_tags_size(tags);
    size_t count = 0;

    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;

        const char *k = nostr_tag_get(t, 0);
        if (!k || strcmp(k, "i") != 0) continue;

        /* Only count valid identity tags (must have colon) */
        const char *v = nostr_tag_get(t, 1);
        if (v && strchr(v, ':') != NULL)
            ++count;
    }
    return count;
}

NostrTag *nostr_nip39_build_tag(const char *platform, const char *identity,
                                 const char *proof_url) {
    if (!platform || !identity) return NULL;

    /* Build "platform:identity" value */
    size_t plen = strlen(platform);
    size_t ilen = strlen(identity);
    char *value = malloc(plen + 1 + ilen + 1);
    if (!value) return NULL;

    memcpy(value, platform, plen);
    value[plen] = ':';
    memcpy(value + plen + 1, identity, ilen + 1);

    NostrTag *tag;
    if (proof_url && *proof_url != '\0')
        tag = nostr_tag_new("i", value, proof_url, NULL);
    else
        tag = nostr_tag_new("i", value, NULL);

    free(value);
    return tag;
}

int nostr_nip39_add(NostrEvent *ev, const char *platform,
                     const char *identity, const char *proof_url) {
    if (!ev || !platform || !identity) return -EINVAL;

    NostrTag *tag = nostr_nip39_build_tag(platform, identity, proof_url);
    if (!tag) return -ENOMEM;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, tag);
    return 0;
}

bool nostr_nip39_find(const NostrEvent *ev, const char *platform,
                       NostrNip39Identity *out) {
    if (!ev || !platform || !out) return false;

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    if (!tags) return false;

    size_t plen = strlen(platform);
    size_t n = nostr_tags_size(tags);

    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        if (!t || nostr_tag_size(t) < 2) continue;

        const char *k = nostr_tag_get(t, 0);
        if (!k || strcmp(k, "i") != 0) continue;

        const char *v = nostr_tag_get(t, 1);
        if (!v) continue;

        /* Check if value starts with "platform:" */
        if (strncmp(v, platform, plen) == 0 && v[plen] == ':') {
            if (parse_identity(t, out))
                return true;
        }
    }
    return false;
}
