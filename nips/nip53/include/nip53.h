#ifndef NIP53_H
#define NIP53_H

#include <nostr/nip53/export.h>

#include <stdint.h>
#include <time.h>

typedef struct {
    char *pub_key;
    char *relay;
    char *role;
} Participant;

typedef struct {
    char *identifier;
    char *title;
    char *summary;
    char *image;
    char *status;
    int current_participants;
    int total_participants;
    char **streaming;
    char **recording;
    time_t starts;
    time_t ends;
    Participant *participants;
    char **hashtags;
    char **relays;
} LiveEvent;

NOSTR_NIP53_API LiveEvent *parse_live_event(const char *event_json);
NOSTR_NIP53_API char *live_event_to_json(LiveEvent *event);
NOSTR_NIP53_API Participant *get_host(LiveEvent *event);
NOSTR_NIP53_API void free_live_event(LiveEvent *event);

#endif // NIP53_H
