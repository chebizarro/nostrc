#ifndef NOSTR_NIP05_H
#define NOSTR_NIP05_H

#include <stdbool.h>

// Define the WellKnownResponse structure
typedef struct {
    char **names;
    char **relays;
} WellKnownResponse;

// Function prototypes
bool is_valid_identifier(const char *input);
int parse_identifier(const char *fullname, char **name, char **domain);
int query_identifier(const char *fullname, char **pubkey, char ***relays);
int fetch_nip05(const char *fullname, WellKnownResponse *response, char **name);
char* normalize_identifier(const char *fullname);

#endif // NOSTR_NIP05_H
