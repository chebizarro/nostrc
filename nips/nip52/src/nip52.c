/**
 * NIP-52: Calendar Events
 *
 * Migrated from jansson to NostrJsonInterface (nostrc-3nj)
 */
#include "nip52.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

static int is_valid_hex(const char *str) {
    if (!str || !*str) return 0;
    while (*str) {
        if (!((*str >= '0' && *str <= '9') || (*str >= 'a' && *str <= 'f') || (*str >= 'A' && *str <= 'F'))) {
            return 0;
        }
        str++;
    }
    return 1;
}

CalendarEvent *parse_calendar_event(const char *event_json) {
    if (!event_json) return NULL;

    CalendarEvent *event = malloc(sizeof(CalendarEvent));
    if (!event) return NULL;
    memset(event, 0, sizeof(CalendarEvent));

    /* Parse event using NostrEvent API */
    NostrEvent *ev = nostr_event_new();
    if (!ev) {
        free(event);
        return NULL;
    }

    if (nostr_event_deserialize(ev, event_json) != 0) {
        nostr_event_free(ev);
        free(event);
        return NULL;
    }

    /* Get kind to determine date vs time based */
    event->kind = (CalendarEventKind)nostr_event_get_kind(ev);

    /* Get tags and iterate */
    NostrTags *tags = nostr_event_get_tags(ev);
    if (!tags) {
        nostr_event_free(ev);
        return event; /* Valid event, just no tags */
    }

    /* Pre-count for proper allocation */
    size_t location_count = 0;
    size_t geohash_count = 0;
    size_t participant_count = 0;
    size_t reference_count = 0;
    size_t hashtag_count = 0;

    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (!tag || nostr_tag_size(tag) < 2) continue;
        const char *key = nostr_tag_get(tag, 0);
        if (!key) continue;

        if (strcmp(key, "location") == 0) location_count++;
        else if (strcmp(key, "g") == 0) geohash_count++;
        else if (strcmp(key, "p") == 0) participant_count++;
        else if (strcmp(key, "r") == 0) reference_count++;
        else if (strcmp(key, "t") == 0) hashtag_count++;
    }

    /* Allocate arrays with null terminator space */
    if (location_count > 0) {
        event->locations = calloc(location_count + 1, sizeof(char *));
    }
    if (geohash_count > 0) {
        event->geohashes = calloc(geohash_count + 1, sizeof(char *));
    }
    if (participant_count > 0) {
        event->participants = calloc(participant_count + 1, sizeof(Participant));
    }
    if (reference_count > 0) {
        event->references = calloc(reference_count + 1, sizeof(char *));
    }
    if (hashtag_count > 0) {
        event->hashtags = calloc(hashtag_count + 1, sizeof(char *));
    }

    /* Reset indices for actual parsing */
    size_t location_idx = 0;
    size_t geohash_idx = 0;
    size_t participant_idx = 0;
    size_t reference_idx = 0;
    size_t hashtag_idx = 0;

    for (size_t i = 0; i < n; i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (!tag || nostr_tag_size(tag) < 2) continue;

        const char *key = nostr_tag_get(tag, 0);
        const char *value = nostr_tag_get(tag, 1);
        if (!key || !value) continue;

        if (strcmp(key, "d") == 0) {
            event->identifier = strdup(value);
        } else if (strcmp(key, "title") == 0) {
            event->title = strdup(value);
        } else if (strcmp(key, "image") == 0) {
            event->image = strdup(value);
        } else if (strcmp(key, "start") == 0 || strcmp(key, "end") == 0) {
            struct tm tm = {0};
            time_t timestamp;

            if (event->kind == TIME_BASED) {
                timestamp = (time_t)atoll(value);
            } else if (event->kind == DATE_BASED) {
                strptime(value, DATE_FORMAT, &tm);
                timestamp = mktime(&tm);
            } else {
                timestamp = 0;
            }

            if (strcmp(key, "start") == 0) {
                event->start = timestamp;
            } else {
                event->end = timestamp;
            }
        } else if (strcmp(key, "location") == 0 && event->locations) {
            event->locations[location_idx++] = strdup(value);
        } else if (strcmp(key, "g") == 0 && event->geohashes) {
            event->geohashes[geohash_idx++] = strdup(value);
        } else if (strcmp(key, "p") == 0 && is_valid_hex(value) && event->participants) {
            event->participants[participant_idx].pub_key = strdup(value);
            if (nostr_tag_size(tag) > 2) {
                const char *relay = nostr_tag_get(tag, 2);
                if (relay) {
                    event->participants[participant_idx].relay = strdup(relay);
                }
                if (nostr_tag_size(tag) > 3) {
                    const char *role = nostr_tag_get(tag, 3);
                    if (role) {
                        event->participants[participant_idx].role = strdup(role);
                    }
                }
            }
            participant_idx++;
        } else if (strcmp(key, "r") == 0 && event->references) {
            event->references[reference_idx++] = strdup(value);
        } else if (strcmp(key, "t") == 0 && event->hashtags) {
            event->hashtags[hashtag_idx++] = strdup(value);
        } else if (strcmp(key, "start_tzid") == 0) {
            event->start_tzid = strdup(value);
        } else if (strcmp(key, "end_tzid") == 0) {
            event->end_tzid = strdup(value);
        }
    }

    nostr_event_free(ev);
    return event;
}

char *calendar_event_to_json(CalendarEvent *event) {
    if (!event) return NULL;

    /* Build NostrEvent with tags */
    NostrEvent *ev = nostr_event_new();
    if (!ev) return NULL;

    /* Set kind based on event type */
    nostr_event_set_kind(ev, (int)event->kind);

    /* Build tags array */
    NostrTags *tags = nostr_tags_new(0);
    if (!tags) {
        nostr_event_free(ev);
        return NULL;
    }

    /* d tag (identifier) */
    if (event->identifier) {
        NostrTag *t = nostr_tag_new("d", event->identifier, NULL);
        if (t) nostr_tags_append(tags, t);
    }

    /* title tag */
    if (event->title) {
        NostrTag *t = nostr_tag_new("title", event->title, NULL);
        if (t) nostr_tags_append(tags, t);
    }

    /* image tag */
    if (event->image) {
        NostrTag *t = nostr_tag_new("image", event->image, NULL);
        if (t) nostr_tags_append(tags, t);
    }

    /* start tag */
    if (event->start != 0) {
        char buf[32];
        if (event->kind == TIME_BASED) {
            snprintf(buf, sizeof(buf), "%lld", (long long)event->start);
        } else {
            strftime(buf, sizeof(buf), DATE_FORMAT, localtime(&event->start));
        }
        NostrTag *t = nostr_tag_new("start", buf, NULL);
        if (t) nostr_tags_append(tags, t);
    }

    /* end tag */
    if (event->end != 0) {
        char buf[32];
        if (event->kind == TIME_BASED) {
            snprintf(buf, sizeof(buf), "%lld", (long long)event->end);
        } else {
            strftime(buf, sizeof(buf), DATE_FORMAT, localtime(&event->end));
        }
        NostrTag *t = nostr_tag_new("end", buf, NULL);
        if (t) nostr_tags_append(tags, t);
    }

    /* location tags */
    for (size_t i = 0; event->locations && event->locations[i]; i++) {
        NostrTag *t = nostr_tag_new("location", event->locations[i], NULL);
        if (t) nostr_tags_append(tags, t);
    }

    /* geohash (g) tags */
    for (size_t i = 0; event->geohashes && event->geohashes[i]; i++) {
        NostrTag *t = nostr_tag_new("g", event->geohashes[i], NULL);
        if (t) nostr_tags_append(tags, t);
    }

    /* participant (p) tags */
    for (size_t i = 0; event->participants && event->participants[i].pub_key; i++) {
        Participant *p = &event->participants[i];
        NostrTag *t = nostr_tag_new("p", p->pub_key, NULL);
        if (t) {
            if (p->relay) {
                nostr_tag_append(t, p->relay);
            }
            if (p->role) {
                if (!p->relay) nostr_tag_append(t, ""); /* placeholder for relay */
                nostr_tag_append(t, p->role);
            }
            nostr_tags_append(tags, t);
        }
    }

    /* reference (r) tags */
    for (size_t i = 0; event->references && event->references[i]; i++) {
        NostrTag *t = nostr_tag_new("r", event->references[i], NULL);
        if (t) nostr_tags_append(tags, t);
    }

    /* hashtag (t) tags */
    for (size_t i = 0; event->hashtags && event->hashtags[i]; i++) {
        NostrTag *t = nostr_tag_new("t", event->hashtags[i], NULL);
        if (t) nostr_tags_append(tags, t);
    }

    /* start_tzid tag */
    if (event->start_tzid) {
        NostrTag *t = nostr_tag_new("start_tzid", event->start_tzid, NULL);
        if (t) nostr_tags_append(tags, t);
    }

    /* end_tzid tag */
    if (event->end_tzid) {
        NostrTag *t = nostr_tag_new("end_tzid", event->end_tzid, NULL);
        if (t) nostr_tags_append(tags, t);
    }

    nostr_event_set_tags(ev, tags);

    /* Serialize event to JSON */
    char *json_str = nostr_event_serialize(ev);
    nostr_event_free(ev);

    return json_str;
}

void free_calendar_event(CalendarEvent *event) {
    if (event) {
        free(event->identifier);
        free(event->title);
        free(event->image);
        for (size_t i = 0; event->locations && event->locations[i]; i++) {
            free(event->locations[i]);
        }
        free(event->locations);
        for (size_t i = 0; event->geohashes && event->geohashes[i]; i++) {
            free(event->geohashes[i]);
        }
        free(event->geohashes);
        for (size_t i = 0; event->participants && event->participants[i].pub_key; i++) {
            free(event->participants[i].pub_key);
            free(event->participants[i].relay);
            free(event->participants[i].role);
        }
        free(event->participants);
        for (size_t i = 0; event->references && event->references[i]; i++) {
            free(event->references[i]);
        }
        free(event->references);
        for (size_t i = 0; event->hashtags && event->hashtags[i]; i++) {
            free(event->hashtags[i]);
        }
        free(event->hashtags);
        free(event->start_tzid);
        free(event->end_tzid);
        free(event);
    }
}
