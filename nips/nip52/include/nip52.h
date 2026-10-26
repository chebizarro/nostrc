#ifndef NIP52_H
#define NIP52_H

#include <nostr/nip52/export.h>

#include <stdint.h>
#include <time.h>

#define DATE_FORMAT "%Y-%m-%d"

typedef enum {
    TIME_BASED = 31923,
    DATE_BASED = 31922
} CalendarEventKind;

typedef struct {
    char *pub_key;
    char *relay;
    char *role;
} Participant;

typedef struct {
    CalendarEventKind kind;
    char *identifier;
    char *title;
    char *image;
    time_t start;
    time_t end;
    char **locations;
    char **geohashes;
    Participant *participants;
    char **references;
    char **hashtags;
    char *start_tzid;
    char *end_tzid;
} CalendarEvent;

NOSTR_NIP52_API CalendarEvent *parse_calendar_event(const char *event_json);
NOSTR_NIP52_API char *calendar_event_to_json(CalendarEvent *event);
NOSTR_NIP52_API void free_calendar_event(CalendarEvent *event);

#endif // NIP52_H
