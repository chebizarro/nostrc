/**
 * NIP-52: Calendar Events — Unit Tests
 *
 * Tests parse_calendar_event(), calendar_event_to_json(), and
 * free_calendar_event().
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nip52.h"

/* ── NULL / empty input handling ─────────────────────────────────────── */

static void test_parse_null(void) {
    CalendarEvent *ev = parse_calendar_event(NULL);
    assert(ev == NULL);
}

static void test_parse_empty_string(void) {
    CalendarEvent *ev = parse_calendar_event("");
    /* Empty string should fail deserialization */
    assert(ev == NULL);
}

static void test_parse_invalid_json(void) {
    CalendarEvent *ev = parse_calendar_event("{not valid json}");
    /* Depending on JSON backend, invalid JSON may partially parse.
     * Either NULL or an empty event with no tags is acceptable. */
    if (ev != NULL) {
        free_calendar_event(ev);
    }
}

/* ── free_calendar_event ─────────────────────────────────────────────── */

static void test_free_null(void) {
    /* Should not crash */
    free_calendar_event(NULL);
}

static void test_free_empty_event(void) {
    CalendarEvent *ev = calloc(1, sizeof(CalendarEvent));
    /* All NULL fields — should not crash */
    free_calendar_event(ev);
}

static void test_free_populated_event(void) {
    CalendarEvent *ev = calloc(1, sizeof(CalendarEvent));
    ev->kind = TIME_BASED;
    ev->identifier = strdup("test-event-1");
    ev->title = strdup("Team Meeting");
    ev->image = strdup("https://example.com/img.png");
    ev->start = 1700000000;
    ev->end = 1700003600;
    ev->start_tzid = strdup("America/New_York");
    ev->end_tzid = strdup("America/New_York");

    /* locations */
    ev->locations = calloc(3, sizeof(char *));
    ev->locations[0] = strdup("Conference Room A");
    ev->locations[1] = strdup("Online");

    /* geohashes */
    ev->geohashes = calloc(2, sizeof(char *));
    ev->geohashes[0] = strdup("u33d8");

    /* participants */
    ev->participants = calloc(2, sizeof(Participant));
    ev->participants[0].pub_key = strdup("aabbccdd");
    ev->participants[0].relay = strdup("wss://relay.example.com");
    ev->participants[0].role = strdup("organizer");

    /* references */
    ev->references = calloc(2, sizeof(char *));
    ev->references[0] = strdup("https://example.com/agenda");

    /* hashtags */
    ev->hashtags = calloc(3, sizeof(char *));
    ev->hashtags[0] = strdup("meeting");
    ev->hashtags[1] = strdup("work");

    /* Should not crash or leak */
    free_calendar_event(ev);
}

/* ── calendar_event_to_json ──────────────────────────────────────────── */

static void test_to_json_null(void) {
    char *json = calendar_event_to_json(NULL);
    assert(json == NULL);
}

static void test_to_json_time_based(void) {
    CalendarEvent ev = {0};
    ev.kind = TIME_BASED;
    ev.identifier = "weekly-standup";
    ev.title = "Weekly Standup";
    ev.start = 1700000000;
    ev.end = 1700001800;

    char *json = calendar_event_to_json(&ev);
    assert(json != NULL);

    /* Verify the JSON contains expected tag values */
    assert(strstr(json, "weekly-standup") != NULL);
    assert(strstr(json, "Weekly Standup") != NULL);
    assert(strstr(json, "1700000000") != NULL);
    assert(strstr(json, "1700001800") != NULL);

    free(json);
}

static void test_to_json_with_participants(void) {
    Participant parts[2];
    memset(parts, 0, sizeof(parts));
    parts[0].pub_key = "aabb";
    parts[0].relay = "wss://relay.example.com";
    parts[0].role = "speaker";
    /* parts[1] is all zeros (NULL pub_key terminates) */

    CalendarEvent ev = {0};
    ev.kind = TIME_BASED;
    ev.identifier = "conf-talk";
    ev.title = "Conference Talk";
    ev.participants = parts;
    ev.start = 1700000000;

    char *json = calendar_event_to_json(&ev);
    assert(json != NULL);
    assert(strstr(json, "aabb") != NULL);
    assert(strstr(json, "wss://relay.example.com") != NULL);
    assert(strstr(json, "speaker") != NULL);
    free(json);
}

static void test_to_json_with_locations_and_hashtags(void) {
    char *locs[] = {"Room 42", "Online", NULL};
    char *tags[] = {"nostr", "meetup", NULL};

    CalendarEvent ev = {0};
    ev.kind = DATE_BASED;
    ev.identifier = "nostr-meetup";
    ev.locations = locs;
    ev.hashtags = tags;

    char *json = calendar_event_to_json(&ev);
    assert(json != NULL);
    assert(strstr(json, "Room 42") != NULL);
    assert(strstr(json, "Online") != NULL);
    assert(strstr(json, "nostr") != NULL);
    assert(strstr(json, "meetup") != NULL);
    free(json);
}

static void test_to_json_with_timezone(void) {
    CalendarEvent ev = {0};
    ev.kind = TIME_BASED;
    ev.identifier = "tz-test";
    ev.start = 1700000000;
    ev.start_tzid = "Europe/Berlin";
    ev.end_tzid = "Europe/Berlin";

    char *json = calendar_event_to_json(&ev);
    assert(json != NULL);
    assert(strstr(json, "Europe/Berlin") != NULL);
    free(json);
}

/* ── CalendarEventKind enum values ───────────────────────────────────── */

static void test_kind_values(void) {
    assert(TIME_BASED == 31923);
    assert(DATE_BASED == 31922);
}

int main(void) {
    /* Parse edge cases */
    test_parse_null();
    test_parse_empty_string();
    test_parse_invalid_json();

    /* Free edge cases */
    test_free_null();
    test_free_empty_event();
    test_free_populated_event();

    /* Serialization */
    test_to_json_null();
    test_to_json_time_based();
    test_to_json_with_participants();
    test_to_json_with_locations_and_hashtags();
    test_to_json_with_timezone();

    /* Enum values */
    test_kind_values();

    printf("nip52 ok\n");
    return 0;
}
