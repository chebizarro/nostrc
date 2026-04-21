/**
 * NIP-53: Live Activities — Unit Tests
 *
 * Tests parse_live_event(), live_event_to_json(), get_host(),
 * and free_live_event().
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nip53.h"

/* ── NULL / empty input handling ─────────────────────────────────────── */

static void test_parse_null(void) {
    LiveEvent *ev = parse_live_event(NULL);
    assert(ev == NULL);
}

static void test_parse_empty_string(void) {
    LiveEvent *ev = parse_live_event("");
    assert(ev == NULL);
}

static void test_parse_invalid_json(void) {
    LiveEvent *ev = parse_live_event("{garbage}");
    /* Depending on JSON backend, invalid JSON may partially parse.
     * Either NULL or an empty event with no tags is acceptable. */
    if (ev != NULL) {
        free_live_event(ev);
    }
}

/* ── get_host ────────────────────────────────────────────────────────── */

static void test_get_host_null(void) {
    assert(get_host(NULL) == NULL);
}

static void test_get_host_no_participants(void) {
    LiveEvent ev = {0};
    assert(get_host(&ev) == NULL);
}

static void test_get_host_found(void) {
    Participant parts[3];
    memset(parts, 0, sizeof(parts));
    parts[0].pub_key = "aabb";
    parts[0].role = "speaker";
    parts[1].pub_key = "ccdd";
    parts[1].role = "host";
    /* parts[2] terminates with NULL pub_key */

    LiveEvent ev = {0};
    ev.participants = parts;

    Participant *host = get_host(&ev);
    assert(host != NULL);
    assert(strcmp(host->pub_key, "ccdd") == 0);
    assert(strcmp(host->role, "host") == 0);
}

static void test_get_host_no_host_role(void) {
    Participant parts[2];
    memset(parts, 0, sizeof(parts));
    parts[0].pub_key = "aabb";
    parts[0].role = "speaker";
    /* No host role */

    LiveEvent ev = {0};
    ev.participants = parts;

    assert(get_host(&ev) == NULL);
}

/* ── free_live_event ─────────────────────────────────────────────────── */

static void test_free_null(void) {
    /* Should not crash */
    free_live_event(NULL);
}

static void test_free_empty_event(void) {
    LiveEvent *ev = calloc(1, sizeof(LiveEvent));
    /* All NULL fields — should not crash */
    free_live_event(ev);
}

static void test_free_populated_event(void) {
    LiveEvent *ev = calloc(1, sizeof(LiveEvent));
    ev->identifier = strdup("live-stream-1");
    ev->title = strdup("Nostr Dev Talk");
    ev->summary = strdup("Discussing NIP-53 implementation");
    ev->image = strdup("https://example.com/thumb.png");
    ev->status = strdup("live");
    ev->starts = 1700000000;
    ev->ends = 1700007200;
    ev->current_participants = 42;
    ev->total_participants = 100;

    /* streaming */
    ev->streaming = calloc(2, sizeof(char *));
    ev->streaming[0] = strdup("https://stream.example.com/live.m3u8");

    /* recording */
    ev->recording = calloc(2, sizeof(char *));
    ev->recording[0] = strdup("https://example.com/recording.mp4");

    /* participants */
    ev->participants = calloc(3, sizeof(Participant));
    ev->participants[0].pub_key = strdup("aabbccdd");
    ev->participants[0].relay = strdup("wss://relay.example.com");
    ev->participants[0].role = strdup("host");
    ev->participants[1].pub_key = strdup("eeff0011");
    ev->participants[1].role = strdup("speaker");

    /* hashtags */
    ev->hashtags = calloc(3, sizeof(char *));
    ev->hashtags[0] = strdup("nostr");
    ev->hashtags[1] = strdup("dev");

    /* relays */
    ev->relays = calloc(2, sizeof(char *));
    ev->relays[0] = strdup("wss://relay.damus.io");

    /* Should not crash or leak */
    free_live_event(ev);
}

/* ── live_event_to_json ──────────────────────────────────────────────── */

static void test_to_json_null(void) {
    char *json = live_event_to_json(NULL);
    assert(json == NULL);
}

static void test_to_json_minimal(void) {
    LiveEvent ev = {0};
    ev.identifier = "stream-1";
    ev.title = "Test Stream";
    ev.status = "live";
    ev.starts = 1700000000;

    char *json = live_event_to_json(&ev);
    assert(json != NULL);
    assert(strstr(json, "stream-1") != NULL);
    assert(strstr(json, "Test Stream") != NULL);
    assert(strstr(json, "live") != NULL);
    assert(strstr(json, "1700000000") != NULL);
    free(json);
}

static void test_to_json_with_streaming_and_recording(void) {
    char *streams[] = {"https://stream1.example.com", "https://stream2.example.com", NULL};
    char *recordings[] = {"https://rec.example.com/1.mp4", NULL};

    LiveEvent ev = {0};
    ev.identifier = "multi-stream";
    ev.streaming = streams;
    ev.recording = recordings;

    char *json = live_event_to_json(&ev);
    assert(json != NULL);
    assert(strstr(json, "https://stream1.example.com") != NULL);
    assert(strstr(json, "https://stream2.example.com") != NULL);
    assert(strstr(json, "https://rec.example.com/1.mp4") != NULL);
    free(json);
}

static void test_to_json_with_participants(void) {
    Participant parts[2];
    memset(parts, 0, sizeof(parts));
    parts[0].pub_key = "aabbccdd";
    parts[0].relay = "wss://relay.example.com";
    parts[0].role = "host";

    LiveEvent ev = {0};
    ev.identifier = "hosted-stream";
    ev.participants = parts;

    char *json = live_event_to_json(&ev);
    assert(json != NULL);
    assert(strstr(json, "aabbccdd") != NULL);
    assert(strstr(json, "wss://relay.example.com") != NULL);
    assert(strstr(json, "host") != NULL);
    free(json);
}

static void test_to_json_with_hashtags_and_relays(void) {
    char *tags[] = {"nostr", "music", NULL};
    char *relays[] = {"wss://relay1.com", "wss://relay2.com", NULL};

    LiveEvent ev = {0};
    ev.identifier = "music-stream";
    ev.hashtags = tags;
    ev.relays = relays;

    char *json = live_event_to_json(&ev);
    assert(json != NULL);
    assert(strstr(json, "nostr") != NULL);
    assert(strstr(json, "music") != NULL);
    assert(strstr(json, "wss://relay1.com") != NULL);
    assert(strstr(json, "wss://relay2.com") != NULL);
    free(json);
}

static void test_to_json_participant_counts(void) {
    LiveEvent ev = {0};
    ev.identifier = "popular-stream";
    ev.current_participants = 150;
    ev.total_participants = 500;

    char *json = live_event_to_json(&ev);
    assert(json != NULL);
    assert(strstr(json, "150") != NULL);
    assert(strstr(json, "500") != NULL);
    free(json);
}

int main(void) {
    /* Parse edge cases */
    test_parse_null();
    test_parse_empty_string();
    test_parse_invalid_json();

    /* get_host */
    test_get_host_null();
    test_get_host_no_participants();
    test_get_host_found();
    test_get_host_no_host_role();

    /* free_live_event */
    test_free_null();
    test_free_empty_event();
    test_free_populated_event();

    /* live_event_to_json */
    test_to_json_null();
    test_to_json_minimal();
    test_to_json_with_streaming_and_recording();
    test_to_json_with_participants();
    test_to_json_with_hashtags_and_relays();
    test_to_json_participant_counts();

    printf("nip53 ok\n");
    return 0;
}
