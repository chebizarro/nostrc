#include "nostr/nip21/nip21.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * Check if str starts with "nostr:" (case-insensitive on the prefix).
 * Returns pointer past the prefix, or NULL if no match.
 */
static const char *strip_prefix(const char *str) {
    if (!str) return NULL;
    /* Case-insensitive check for "nostr:" */
    if ((str[0] == 'n' || str[0] == 'N') &&
        (str[1] == 'o' || str[1] == 'O') &&
        (str[2] == 's' || str[2] == 'S') &&
        (str[3] == 't' || str[3] == 'T') &&
        (str[4] == 'r' || str[4] == 'R') &&
        str[5] == ':') {
        return str + NOSTR_NIP21_PREFIX_LEN;
    }
    return NULL;
}

bool nostr_nip21_is_uri(const char *str) {
    if (!str) return false;

    const char *bech32 = strip_prefix(str);
    if (!bech32 || *bech32 == '\0') return false;

    /* Validate the bech32 portion with NIP-19 inspect */
    NostrBech32Type type = NOSTR_B32_UNKNOWN;
    if (nostr_nip19_inspect(bech32, &type) != 0)
        return false;

    return type != NOSTR_B32_UNKNOWN;
}

int nostr_nip21_parse(const char *uri, NostrBech32Type *out_type,
                       const char **out_bech32) {
    if (!uri || !out_type || !out_bech32) return -EINVAL;

    const char *bech32 = strip_prefix(uri);
    if (!bech32 || *bech32 == '\0') return -EINVAL;

    NostrBech32Type type = NOSTR_B32_UNKNOWN;
    if (nostr_nip19_inspect(bech32, &type) != 0)
        return -EINVAL;

    if (type == NOSTR_B32_UNKNOWN)
        return -EINVAL;

    *out_type = type;
    *out_bech32 = bech32;
    return 0;
}

char *nostr_nip21_build(const char *bech32) {
    if (!bech32 || *bech32 == '\0') return NULL;

    size_t bech_len = strlen(bech32);
    char *uri = malloc(NOSTR_NIP21_PREFIX_LEN + bech_len + 1);
    if (!uri) return NULL;

    memcpy(uri, NOSTR_NIP21_PREFIX, NOSTR_NIP21_PREFIX_LEN);
    memcpy(uri + NOSTR_NIP21_PREFIX_LEN, bech32, bech_len + 1);
    return uri;
}

/*
 * Helper: encode with NIP-19, then prepend "nostr:" prefix.
 * Takes ownership of the bech32 string on success.
 */
static char *wrap_bech32(char *bech32) {
    if (!bech32) return NULL;
    char *uri = nostr_nip21_build(bech32);
    free(bech32);
    return uri;
}

char *nostr_nip21_build_npub(const uint8_t pubkey[32]) {
    if (!pubkey) return NULL;
    char *bech = NULL;
    if (nostr_nip19_encode_npub(pubkey, &bech) != 0)
        return NULL;
    return wrap_bech32(bech);
}

char *nostr_nip21_build_note(const uint8_t event_id[32]) {
    if (!event_id) return NULL;
    char *bech = NULL;
    if (nostr_nip19_encode_note(event_id, &bech) != 0)
        return NULL;
    return wrap_bech32(bech);
}

char *nostr_nip21_build_nprofile(const NostrProfilePointer *p) {
    if (!p) return NULL;
    char *bech = NULL;
    if (nostr_nip19_encode_nprofile(p, &bech) != 0)
        return NULL;
    return wrap_bech32(bech);
}

char *nostr_nip21_build_nevent(const NostrEventPointer *e) {
    if (!e) return NULL;
    char *bech = NULL;
    if (nostr_nip19_encode_nevent(e, &bech) != 0)
        return NULL;
    return wrap_bech32(bech);
}

char *nostr_nip21_build_naddr(const NostrEntityPointer *a) {
    if (!a) return NULL;
    char *bech = NULL;
    if (nostr_nip19_encode_naddr(a, &bech) != 0)
        return NULL;
    return wrap_bech32(bech);
}
