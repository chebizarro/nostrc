/**
 * NIP-53: Live Activities
 *
 * Migrated from jansson to NostrJsonInterface (nostrc-3nj)
 */
#include "nip53.h"
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

/* Helper to count null-terminated array entries */
static size_t count_array(char **arr) {
    if (!arr) return 0;
    size_t n = 0;
    while (arr[n]) n++;
    return n;
}

/* Helper to count participants */
static size_t count_participants(Participant *p) {
    if (!p) return 0;
    size_t n = 0;
    while (p[n].pub_key) n++;
    return n;
}

LiveEvent *parse_live_event(const char *event_json) {
    if (!event_json) return NULL;

    LiveEvent *event = malloc(sizeof(LiveEvent));
    if (!event) return NULL;
    memset(event, 0, sizeof(LiveEvent));

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

    /* Get tags and iterate */
    NostrTags *tags = nostr_event_get_tags(ev);
    if (!tags) {
        nostr_event_free(ev);
        return event; /* Valid event, just no tags */
    }

    /* Pre-count for proper allocation */
    size_t streaming_count = 0;
    size_t recording_count = 0;
    size_t participant_count = 0;
    size_t hashtag_count = 0;
    size_t relay_count = 0;

    size_t n = nostr_tags_size(tags);
    for (size_t i = 0; i < n; i++) {
        NostrTag *tag = nostr_tags_get(tags, i);
        if (!tag || nostr_tag_size(tag) < 2) continue;
        const char *key = nostr_tag_get(tag, 0);
        if (!key) continue;

        if (strcmp(key, "streaming") == 0) streaming_count++;
        else if (strcmp(key, "recording") == 0) recording_count++;
        else if (strcmp(key, "p") == 0) participant_count++;
        else if (strcmp(key, "t") == 0) hashtag_count++;
        else if (strcmp(key, "relay") == 0) relay_count++;
    }

    /* Allocate arrays with null terminator space */
    if (streaming_count > 0) {
        event->streaming = calloc(streaming_count + 1, sizeof(char *));
    }
    if (recording_count > 0) {
        event->recording = calloc(recording_count + 1, sizeof(char *));
    }
    if (participant_count > 0) {
        event->participants = calloc(participant_count + 1, sizeof(Participant));
    }
    if (hashtag_count > 0) {
        event->hashtags = calloc(hashtag_count + 1, sizeof(char *));
    }
    if (relay_count > 0) {
        event->relays = calloc(relay_count + 1, sizeof(char *));
    }

    /* Reset indices for actual parsing */
    size_t streaming_idx = 0;
    size_t recording_idx = 0;
    size_t participant_idx = 0;
    size_t hashtag_idx = 0;
    size_t relay_idx = 0;

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
        } else if (strcmp(key, "summary") == 0) {
            event->summary = strdup(value);
        } else if (strcmp(key, "image") == 0) {
            event->image = strdup(value);
        } else if (strcmp(key, "status") == 0) {
            event->status = strdup(value);
        } else if (strcmp(key, "start") == 0) {
            event->starts = (time_t)atoll(value);
        } else if (strcmp(key, "end") == 0) {
            event->ends = (time_t)atoll(value);
        } else if (strcmp(key, "streaming") == 0 && event->streaming) {
            event->streaming[streaming_idx++] = strdup(value);
        } else if (strcmp(key, "recording") == 0 && event->recording) {
            event->recording[recording_idx++] = strdup(value);
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
        } else if (strcmp(key, "relay") == 0 && event->relays) {
            event->relays[relay_idx++] = strdup(value);
        } else if (strcmp(key, "t") == 0 && event->hashtags) {
            event->hashtags[hashtag_idx++] = strdup(value);
        } else if (strcmp(key, "current_participants") == 0) {
            event->current_participants = atoi(value);
        } else if (strcmp(key, "total_participants") == 0) {
            event->total_participants = atoi(value);
        }
    }

    nostr_event_free(ev);
    return event;
}

char *live_event_to_json(LiveEvent *event) {
    if (!event) return NULL;

    /* Build NostrEvent with tags */
    NostrEvent *ev = nostr_event_new();
    if (!ev) return NULL;

    /* Set kind 30311 for live events */
    nostr_event_set_kind(ev, 30311);

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

    /* summary tag */
    if (event->summary) {
        NostrTag *t = nostr_tag_new("summary", event->summary, NULL);
        if (t) nostr_tags_append(tags, t);
    }

    /* image tag */
    if (event->image) {
        NostrTag *t = nostr_tag_new("image", event->image, NULL);
        if (t) nostr_tags_append(tags, t);
    }

    /* status tag */
    if (event->status) {
        NostrTag *t = nostr_tag_new("status", event->status, NULL);
        if (t) nostr_tags_append(tags, t);
    }

    /* start tag */
    if (event->starts != 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)event->starts);
        NostrTag *t = nostr_tag_new("start", buf, NULL);
        if (t) nostr_tags_append(tags, t);
    }

    /* end tag */
    if (event->ends != 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)event->ends);
        NostrTag *t = nostr_tag_new("end", buf, NULL);
        if (t) nostr_tags_append(tags, t);
    }

    /* streaming tags */
    for (size_t i = 0; event->streaming && event->streaming[i]; i++) {
        NostrTag *t = nostr_tag_new("streaming", event->streaming[i], NULL);
        if (t) nostr_tags_append(tags, t);
    }

    /* recording tags */
    for (size_t i = 0; event->recording && event->recording[i]; i++) {
        NostrTag *t = nostr_tag_new("recording", event->recording[i], NULL);
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

    /* hashtag (t) tags */
    for (size_t i = 0; event->hashtags && event->hashtags[i]; i++) {
        NostrTag *t = nostr_tag_new("t", event->hashtags[i], NULL);
        if (t) nostr_tags_append(tags, t);
    }

    /* current_participants tag */
    if (event->current_participants != 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", event->current_participants);
        NostrTag *t = nostr_tag_new("current_participants", buf, NULL);
        if (t) nostr_tags_append(tags, t);
    }

    /* total_participants tag */
    if (event->total_participants != 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", event->total_participants);
        NostrTag *t = nostr_tag_new("total_participants", buf, NULL);
        if (t) nostr_tags_append(tags, t);
    }

    /* relay tags */
    for (size_t i = 0; event->relays && event->relays[i]; i++) {
        NostrTag *t = nostr_tag_new("relay", event->relays[i], NULL);
        if (t) nostr_tags_append(tags, t);
    }

    nostr_event_set_tags(ev, tags);

    /* Serialize event to JSON */
    char *json_str = nostr_event_serialize(ev);
    nostr_event_free(ev);

    return json_str;
}

Participant *get_host(LiveEvent *event) {
    if (!event) return NULL;
    for (size_t i = 0; event->participants && event->participants[i].pub_key; i++) {
        if (event->participants[i].role && strcmp(event->participants[i].role, "host") == 0) {
            return &event->participants[i];
        }
    }
    return NULL;
}

void free_live_event(LiveEvent *event) {
    if (event) {
        free(event->identifier);
        free(event->title);
        free(event->summary);
        free(event->image);
        free(event->status);
        for (size_t i = 0; event->streaming && event->streaming[i]; i++) {
            free(event->streaming[i]);
        }
        free(event->streaming);
        for (size_t i = 0; event->recording && event->recording[i]; i++) {
            free(event->recording[i]);
        }
        free(event->recording);
        for (size_t i = 0; event->participants && event->participants[i].pub_key; i++) {
            free(event->participants[i].pub_key);
            free(event->participants[i].relay);
            free(event->participants[i].role);
        }
        free(event->participants);
        for (size_t i = 0; event->hashtags && event->hashtags[i]; i++) {
            free(event->hashtags[i]);
        }
        free(event->hashtags);
        for (size_t i = 0; event->relays && event->relays[i]; i++) {
            free(event->relays[i]);
        }
        free(event->relays);
        free(event);
    }
}
