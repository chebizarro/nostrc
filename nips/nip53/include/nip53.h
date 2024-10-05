#ifndef NIP53_H
#define NIP53_H

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

LiveEvent *parse_live_event(const char *event_json);
char *live_event_to_json(LiveEvent *event);
Participant *get_host(LiveEvent *event);
void free_live_event(LiveEvent *event);

#endif // NIP53_H
