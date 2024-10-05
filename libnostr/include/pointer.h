#ifndef POINTER_H
#define POINTER_H

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

// ProfilePointer struct
typedef struct {
    char *public_key;
    char **relays;
    size_t relays_count;
} ProfilePointer;

// EventPointer struct
typedef struct {
    char *id;
    char **relays;
    size_t relays_count;
    char *author;
    int kind;
} EventPointer;

// EntityPointer struct
typedef struct {
    char *public_key;
    int kind;
    char *identifier;
    char **relays;
    size_t relays_count;
} EntityPointer;

// Function prototypes for ProfilePointer
ProfilePointer *create_profile_pointer();
void free_profile_pointer(ProfilePointer *ptr);

// Function prototypes for EventPointer
EventPointer *create_event_pointer();
void free_event_pointer(EventPointer *ptr);

// Function prototypes for EntityPointer
EntityPointer *create_entity_pointer();
void free_entity_pointer(EntityPointer *ptr);

#endif // POINTER_H