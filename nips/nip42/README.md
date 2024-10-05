# NIP-42 Authentication

NIP-42 defines a way for clients to authenticate to relays by signing an ephemeral event. This implementation provides functions to create unsigned authentication events and validate authentication events.

### Functions

#### `nip42_create_unsigned_auth_event`

Creates an event which should be sent via an "AUTH" command. If the authentication succeeds, the user will be authenticated as the provided pubkey.

```c
nip42_Event* nip42_create_unsigned_auth_event(const char* challenge, const char* pubkey, const char* relay_url);
```

- **Parameters:**
  - `challenge`: The challenge string received from the relay.
  - `pubkey`: The public key of the user.
  - `relay_url`: The URL of the relay.

- **Returns:**
  - A dynamically allocated `nip42_Event` structure. Use `nip42_event_free` to free the allocated memory.

#### `nip42_validate_auth_event`

Validates whether an event is a valid NIP-42 authentication event for the given challenge and relay URL.

```c
bool nip42_validate_auth_event(const nip42_Event* event, const char* challenge, const char* relay_url, char** pubkey);
```

- **Parameters:**
  - `event`: The `nip42_Event` structure to validate.
  - `challenge`: The challenge string to validate against.
  - `relay_url`: The relay URL to validate against.
  - `pubkey`: Pointer to a string where the public key will be stored if the event is valid.

- **Returns:**
  - `true` if the event is valid.
  - `false` otherwise.

#### `nip42_event_free`

Frees the memory allocated for a `nip42_Event` structure.

```c
void nip42_event_free(nip42_Event* event);
```

- **Parameters:**
  - `event`: Pointer to the `nip42_Event` structure whose memory is to be freed.

### Example Usage

```c
#include "nip42.h"
#include <stdio.h>

int main() {
    // Create an unsigned authentication event
    nip42_Event* auth_event = nip42_create_unsigned_auth_event("challenge-string", "public-key", "https://relay.example.com");
    if (!auth_event) {
        printf("Failed to create authentication event.\n");
        return 1;
    }

    // Validate the authentication event
    char* pubkey = NULL;
    if (nip42_validate_auth_event(auth_event, "challenge-string", "https://relay.example.com", &pubkey)) {
        printf("Authentication event is valid. Pubkey: %s\n", pubkey);
        free(pubkey);
    } else {
        printf("Authentication event is invalid.\n");
    }

    // Free the authentication event
    nip42_event_free(auth_event);

    return 0;
}
```
