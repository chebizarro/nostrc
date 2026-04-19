#include "nostr/nip24/nip24.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * Simple JSON string value extractor.
 * Finds "key":"value" in JSON and returns a malloc'd copy of value.
 * Handles basic escape sequences (\", \\, \n, \t, \/).
 * Returns NULL if key not found.
 */
static char *json_get_string(const char *json, const char *key) {
    if (!json || !key) return NULL;

    size_t klen = strlen(key);

    /* Search for "key" pattern */
    const char *p = json;
    while ((p = strstr(p, "\"")) != NULL) {
        ++p; /* past opening quote */

        /* Check if this matches the key */
        if (strncmp(p, key, klen) != 0 || p[klen] != '"') {
            /* Skip to end of this string */
            while (*p && *p != '"') {
                if (*p == '\\' && *(p + 1)) p += 2;
                else ++p;
            }
            if (*p == '"') ++p;
            continue;
        }

        /* Found key — skip past key closing quote */
        p += klen + 1;

        /* Skip whitespace and colon */
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
        if (*p != ':') continue;
        ++p;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;

        /* Expect opening quote for string value */
        if (*p != '"') continue;
        ++p;

        /* Extract value until closing quote */
        const char *start = p;
        while (*p && *p != '"') {
            if (*p == '\\' && *(p + 1)) p += 2;
            else ++p;
        }

        size_t vlen = (size_t)(p - start);
        char *result = malloc(vlen + 1);
        if (!result) return NULL;

        /* Copy with basic unescape */
        size_t out = 0;
        for (const char *s = start; s < p; ++s) {
            if (*s == '\\' && s + 1 < p) {
                ++s;
                switch (*s) {
                    case '"':  result[out++] = '"'; break;
                    case '\\': result[out++] = '\\'; break;
                    case 'n':  result[out++] = '\n'; break;
                    case 't':  result[out++] = '\t'; break;
                    case '/':  result[out++] = '/'; break;
                    case 'r':  result[out++] = '\r'; break;
                    default:   result[out++] = '\\'; result[out++] = *s; break;
                }
            } else {
                result[out++] = *s;
            }
        }
        result[out] = '\0';
        return result;
    }

    return NULL;
}

/*
 * Check if a JSON boolean field is true.
 */
static bool json_get_bool(const char *json, const char *key) {
    if (!json || !key) return false;

    size_t klen = strlen(key);

    /* Search for "key":true pattern */
    const char *p = json;
    while ((p = strstr(p, key)) != NULL) {
        /* Verify it's a proper key (preceded by ") */
        if (p > json && *(p - 1) == '"') {
            const char *after = p + klen;
            if (*after == '"') {
                ++after;
                while (*after == ' ' || *after == '\t') ++after;
                if (*after == ':') {
                    ++after;
                    while (*after == ' ' || *after == '\t') ++after;
                    if (strncmp(after, "true", 4) == 0)
                        return true;
                }
            }
        }
        p += klen;
    }
    return false;
}

int nostr_nip24_parse_profile(const char *json, NostrNip24Profile *out) {
    if (!json || !out) return -EINVAL;
    memset(out, 0, sizeof(*out));

    out->name = json_get_string(json, "name");
    out->display_name = json_get_string(json, "display_name");
    out->about = json_get_string(json, "about");
    out->picture = json_get_string(json, "picture");
    out->banner = json_get_string(json, "banner");
    out->website = json_get_string(json, "website");
    out->nip05 = json_get_string(json, "nip05");
    out->lud06 = json_get_string(json, "lud06");
    out->lud16 = json_get_string(json, "lud16");
    out->bot = json_get_bool(json, "bot");

    return 0;
}

void nostr_nip24_profile_free(NostrNip24Profile *profile) {
    if (!profile) return;
    free(profile->name);
    free(profile->display_name);
    free(profile->about);
    free(profile->picture);
    free(profile->banner);
    free(profile->website);
    free(profile->nip05);
    free(profile->lud06);
    free(profile->lud16);
    memset(profile, 0, sizeof(*profile));
}

char *nostr_nip24_get_display_name(const char *json) {
    if (!json) return NULL;

    char *dn = json_get_string(json, "display_name");
    if (dn && *dn != '\0') return dn;
    free(dn);

    return json_get_string(json, "name");
}

bool nostr_nip24_is_bot(const char *json) {
    return json_get_bool(json, "bot");
}
