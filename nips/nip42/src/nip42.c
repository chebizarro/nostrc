#include "nip42.h"
#include "nostr.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <curl/curl.h>

// Helper function to create an unsigned authentication event.
nip42_Event* nip42_create_unsigned_auth_event(const char* challenge, const char* pubkey, const char* relay_url) {
    nip42_Event* event = (nip42_Event*)malloc(sizeof(nip42_Event));
    if (!event) {
        return NULL;
    }

    event->pubkey = strdup(pubkey);
    event->created_at = time(NULL);
    event->kind = 22242; // nostr.KindClientAuthentication
    event->tags.count = 2;
    event->tags.items = (nostr_Tag*)malloc(2 * sizeof(nostr_Tag));
    if (!event->tags.items) {
        free(event);
        return NULL;
    }

    event->tags.items[0].count = 2;
    event->tags.items[0].fields[0] = strdup("relay");
    event->tags.items[0].fields[1] = strdup(relay_url);

    event->tags.items[1].count = 2;
    event->tags.items[1].fields[0] = strdup("challenge");
    event->tags.items[1].fields[1] = strdup(challenge);

    event->content = strdup("");

    return event;
}

// Helper function to parse URL.
bool parse_url(const char* input, CURLU** url) {
    CURLU* u = curl_url();
    CURLUcode result = curl_url_set(u, CURLUPART_URL, input, 0);
    if (result != CURLUE_OK) {
        curl_url_cleanup(u);
        return false;
    }
    *url = u;
    return true;
}

// Validate an authentication event.
bool nip42_validate_auth_event(const nip42_Event* event, const char* challenge, const char* relay_url, char** pubkey) {
    if (event->kind != 22242) { // nostr.KindClientAuthentication
        return false;
    }

    // Check challenge
    bool challenge_ok = false;
    for (int i = 0; i < event->tags.count; ++i) {
        if (strcmp(event->tags.items[i].fields[0], "challenge") == 0 &&
            strcmp(event->tags.items[i].fields[1], challenge) == 0) {
            challenge_ok = true;
            break;
        }
    }
    if (!challenge_ok) {
        return false;
    }

    // Check relay URL
    CURLU* expected = NULL;
    CURLU* found = NULL;
    if (!parse_url(relay_url, &expected) || !parse_url(event->tags.items[0].fields[1], &found)) {
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

    bool url_match = (strcmp(expected_scheme, found_scheme) == 0) &&
                     (strcmp(expected_host, found_host) == 0) &&
                     (strcmp(expected_path, found_path) == 0);

    curl_free(expected_scheme);
    curl_free(found_scheme);
    curl_free(expected_host);
    curl_free(found_host);
    curl_free(expected_path);
    curl_free(found_path);
    curl_url_cleanup(expected);
    curl_url_cleanup(found);

    if (!url_match) {
        return false;
    }

    // Check created_at
    time_t now = time(NULL);
    if (event->created_at > now + 600 || event->created_at < now - 600) {
        return false;
    }

    // Check signature
    if (!nostr_check_signature(event->id, event->pubkey, event->sig)) {
        return false;
    }

    *pubkey = strdup(event->pubkey);
    return true;
}

// Free memory allocated for nip42_Event.
void nip42_event_free(nip42_Event* event) {
    if (!event) {
        return;
    }

    free(event->id);
    free(event->pubkey);
    free(event->content);
    for (int i = 0; i < event->tags.count; ++i) {
        for (int j = 0; j < event->tags.items[i].count; ++j) {
            free(event->tags.items[i].fields[j]);
        }
    }
    free(event->tags.items);
    free(event);
}
