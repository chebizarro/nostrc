#include "nip42.h"
#include "nostr.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <curl/curl.h>

static const char *find_tag_value(NostrTags *tags, const char *key) {
    if (!tags || !key) return NULL;
    for (size_t i = 0; i < nostr_tags_size(tags); ++i) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (!tag || nostr_tag_size(tag) < 2) continue;
        const char *k = nostr_tag_get(tag, 0);
        if (k && strcmp(k, key) == 0) return nostr_tag_get(tag, 1);
    }
    return NULL;
}

// Helper function to create an unsigned authentication event.
nip42_Event* nip42_create_unsigned_auth_event(const char* challenge, const char* pubkey, const char* relay_url) {
    if (!challenge || !pubkey || !relay_url) return NULL;

    nip42_Event* event = (nip42_Event*)calloc(1, sizeof(nip42_Event));
    if (!event) return NULL;

    event->pubkey = strdup(pubkey);
    event->created_at = time(NULL);
    event->kind = NIP42_KIND_CLIENT_AUTHENTICATION;
    event->content = strdup("");
    event->id = NULL;
    event->sig = NULL;

    event->tags = nostr_tags_new(0);
    if (!event->pubkey || !event->content || !event->tags) {
        nip42_event_free(event);
        return NULL;
    }

    NostrTag *relay_tag = nostr_tag_new("relay", relay_url, NULL);
    NostrTag *challenge_tag = nostr_tag_new("challenge", challenge, NULL);
    if (!relay_tag || !challenge_tag) {
        if (relay_tag) nostr_tag_free(relay_tag);
        if (challenge_tag) nostr_tag_free(challenge_tag);
        nip42_event_free(event);
        return NULL;
    }
    nostr_tags_append(event->tags, relay_tag);
    nostr_tags_append(event->tags, challenge_tag);

    return event;
}

// Helper function to parse URL.
static bool parse_url(const char* input, CURLU** url) {
    if (!input || !url) return false;
    CURLU* u = curl_url();
    if (!u) return false;
    CURLUcode result = curl_url_set(u, CURLUPART_URL, input, 0);
    if (result != CURLUE_OK) {
        curl_url_cleanup(u);
        return false;
    }
    *url = u;
    return true;
}

static bool relay_url_matches(const char *expected_url, const char *found_url) {
    CURLU* expected = NULL;
    CURLU* found = NULL;
    if (!parse_url(expected_url, &expected) || !parse_url(found_url, &found)) {
        if (expected) curl_url_cleanup(expected);
        if (found) curl_url_cleanup(found);
        return false;
    }

    char* expected_scheme = NULL;
    char* found_scheme = NULL;
    char* expected_host = NULL;
    char* found_host = NULL;
    char* expected_path = NULL;
    char* found_path = NULL;

    curl_url_get(expected, CURLUPART_SCHEME, &expected_scheme, 0);
    curl_url_get(found, CURLUPART_SCHEME, &found_scheme, 0);
    curl_url_get(expected, CURLUPART_HOST, &expected_host, 0);
    curl_url_get(found, CURLUPART_HOST, &found_host, 0);
    curl_url_get(expected, CURLUPART_PATH, &expected_path, 0);
    curl_url_get(found, CURLUPART_PATH, &found_path, 0);

    bool url_match = expected_scheme && found_scheme && expected_host && found_host && expected_path && found_path &&
                     strcmp(expected_scheme, found_scheme) == 0 &&
                     strcmp(expected_host, found_host) == 0 &&
                     strcmp(expected_path, found_path) == 0;

    curl_free(expected_scheme);
    curl_free(found_scheme);
    curl_free(expected_host);
    curl_free(found_host);
    curl_free(expected_path);
    curl_free(found_path);
    curl_url_cleanup(expected);
    curl_url_cleanup(found);
    return url_match;
}

// Validate an authentication event.
bool nip42_validate_auth_event(const nip42_Event* event, const char* challenge, const char* relay_url, char** pubkey) {
    if (pubkey) *pubkey = NULL;
    if (!event || !challenge || !relay_url) return false;
    if (event->kind != NIP42_KIND_CLIENT_AUTHENTICATION) return false;
    if (!event->id || !event->pubkey || !event->sig || !event->content || !event->tags) return false;

    const char *event_challenge = find_tag_value(event->tags, "challenge");
    if (!event_challenge || strcmp(event_challenge, challenge) != 0) return false;

    const char *event_relay = find_tag_value(event->tags, "relay");
    if (!event_relay || !relay_url_matches(relay_url, event_relay)) return false;

    time_t now = time(NULL);
    if (event->created_at > now + 600 || event->created_at < now - 600) return false;

    NostrEvent check_event = {
        .id = event->id,
        .pubkey = event->pubkey,
        .created_at = event->created_at,
        .kind = event->kind,
        .tags = event->tags,
        .content = event->content,
        .sig = event->sig,
        .extra = NULL,
    };
    if (!nostr_event_check_signature(&check_event)) return false;

    if (pubkey) {
        *pubkey = strdup(event->pubkey);
        if (!*pubkey) return false;
    }
    return true;
}

bool nip42_parse_challenge(const char* raw_msg, char** out_challenge) {
    if (!raw_msg || !out_challenge) return false;
    *out_challenge = NULL;

    const char *p = raw_msg;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '[') {
        if (*p == '\0') return false;
        *out_challenge = strdup(p);
        return *out_challenge != NULL;
    }

    NostrEnvelope *env = nostr_envelope_parse(raw_msg);
    if (!env) return false;

    bool ok = false;
    if (nostr_envelope_get_type(env) == NOSTR_ENVELOPE_AUTH) {
        const char *challenge = nostr_auth_envelope_get_challenge((const NostrAuthEnvelope *)env);
        if (challenge && *challenge) {
            *out_challenge = strdup(challenge);
            ok = (*out_challenge != NULL);
        }
    }
    nostr_envelope_free(env);
    return ok;
}

bool nip42_build_auth_response(const char* challenge, const char* relay_url,
                               const char* secret_key_hex, char** out_json) {
    if (!challenge || !relay_url || !secret_key_hex || !out_json) return false;
    *out_json = NULL;

    NostrEvent *ev = nostr_event_new();
    if (!ev) return false;
    nostr_event_set_kind(ev, NIP42_KIND_CLIENT_AUTHENTICATION);
    nostr_event_set_created_at(ev, (int64_t)time(NULL));
    nostr_event_set_content(ev, "");

    NostrTags *tags = nostr_tags_new(0);
    if (!tags) { nostr_event_free(ev); return false; }
    NostrTag *relay_tag = nostr_tag_new("relay", relay_url, NULL);
    NostrTag *challenge_tag = nostr_tag_new("challenge", challenge, NULL);
    if (!relay_tag || !challenge_tag) {
        if (relay_tag) nostr_tag_free(relay_tag);
        if (challenge_tag) nostr_tag_free(challenge_tag);
        nostr_tags_free(tags);
        nostr_event_free(ev);
        return false;
    }
    nostr_tags_append(tags, relay_tag);
    nostr_tags_append(tags, challenge_tag);
    nostr_event_set_tags(ev, tags); /* takes ownership */

    if (nostr_event_sign(ev, secret_key_hex) != 0) {
        nostr_event_free(ev);
        return false;
    }

    NostrAuthEnvelope auth = {
        .base = { .type = NOSTR_ENVELOPE_AUTH },
        .challenge = NULL,
        .event = ev,
    };
    char *frame = nostr_envelope_serialize_compact((const NostrEnvelope *)&auth);
    if (!frame) {
        char *event_json = nostr_event_serialize_compact(ev);
        if (event_json) {
            size_t len = strlen(event_json) + 11;
            frame = (char *)malloc(len);
            if (frame) snprintf(frame, len, "[\"AUTH\",%s]", event_json);
            free(event_json);
        }
    }
    nostr_event_free(ev);

    if (!frame) return false;
    *out_json = frame;
    return true;
}

// Free memory allocated for nip42_Event.
void nip42_event_free(nip42_Event* event) {
    if (!event) return;

    free(event->id);
    free(event->pubkey);
    free(event->content);
    free(event->sig);
    if (event->tags) nostr_tags_free(event->tags);
    free(event);
}
