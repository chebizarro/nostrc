# Nostr C Library

The Nostr C library provides an implementation of the Nostr protocol, including various NIPs (Nostr Improvement Proposals). This library aims to be highly portable, suitable for use in IoT environments, and provides bindings for integration with the GNOME desktop environment.

## Features

- Nostr event handling
- JSON (de)serialization with optional NSON support
- NIP implementations (e.g., NIP-04, NIP-05, NIP-13, NIP-19, NIP-29, NIP-31, NIP-34)
- Optional memory management handled by the library
- FUSE filesystem for Nostr relays

## Installation

### Dependencies

- C compiler (GCC/Clang)
- CMake
- libsecp256k1
- libjansson (optional, for JSON parsing)

### Building

```sh
git clone https://github.com/chebizarro/nostrc.git
cd nostrc
mkdir build
cd build
cmake ..
make
sudo make install
```

## Usage

### Including the Library

Include the necessary headers in your project:

```c
#include "nostr.h"
#include "nson.h"
```

### Example: Unmarshaling an Event

```c
#include "nson.h"
#include <stdio.h>

int main() {
    const char* nson_data = "{\"id\":\"abc123\",\"pubkey\":\"def456\",\"sig\":\"ghi789\",\"created_at\":1627845443,\"nson\":\"...\",\"kind\":1,\"content\":\"hello world\",\"tags\":[...]}";

    nson_Event event;
    if (nson_unmarshal(nson_data, &event) == 0) {
        printf("Event ID: %s\n", event.id);
        printf("Event PubKey: %s\n", event.pubkey);
        printf("Event Signature: %s\n", event.sig);
        printf("Event Created At: %ld\n", event.created_at);
        printf("Event Kind: %d\n", event.kind);
        printf("Event Content: %s\n", event.content);
        nson_event_free(&event);
    } else {
        printf("Failed to unmarshal NSON data.\n");
    }

    return 0;
}
```

### Example: Marshaling an Event

```c
#include "nson.h"
#include <stdio.h>
#include <stdlib.h>

int main() {
    nson_Event new_event = {
        .id = "abc123",
        .pubkey = "def456",
        .sig = "ghi789",
        .created_at = 1627845443,
        .kind = 1,
        .content = "hello world",
        .tags_count = 1,
        .tags = malloc(sizeof(nostr_Tag))
    };
    new_event.tags[0].fields_count = 2;
    new_event.tags[0].fields[0] = strdup("tag1");
    new_event.tags[0].fields[1] = strdup("tag2");

    char* nson_string = nson_marshal(&new_event);
    if (nson_string != NULL) {
        printf("NSON String: %s\n", nson_string);
        free(nson_string);
    } else {
        printf("Failed to marshal event to NSON.\n");
    }

    nson_event_free(&new_event);
    return 0;
}
```

## Memory Management

The library provides functions to handle memory allocation and deallocation:

- `nson_unmarshal` dynamically allocates memory for the event fields.
- `nson_event_free` frees the allocated memory for an `nson_Event`.

Always ensure to free the memory using `nson_event_free` to avoid memory leaks.

## NIP Implementations

This library includes various Nostr Improvement Proposals (NIPs):

- **NIP-04**: Encrypted Direct Messages
- **NIP-05**: Mapping Nostr keys to DNS-based identifiers
- **NIP-13**: Proof-of-Work
- **NIP-19**: Bech32-encoded entities
- **NIP-29**: Simple group management
- **NIP-31**: Alternative content
- **NIP-34**: GitHub-like repository management on Nostr

## GNOME Integration

The library includes support for GNOME Seahorse for key management and integration with the GNOME desktop for login and notifications.

### Seahorse Plugin

The Seahorse plugin provides functions for managing Nostr keys directly from Seahorse.

### GNOME Login Integration

Integrate Nostr keys for user authentication on the GNOME desktop. Users can login using a Nostr private key, external USB key device, or a QR code scan.

## FUSE Filesystem

The library includes a FUSE implementation to mount a Nostr relay and interact with stored events as JSON files.

## Continuous Integration and Delivery

A comprehensive CI/CD pipeline is set up for the project using GitHub Actions. The pipeline ensures that all components are tested and built for various target environments.

### CI/CD Pipeline

1. **Linting**: Runs `clang-tidy` for code quality checks.
2. **Building**: Compiles the library for different platforms.
3. **Testing**: Runs unit tests to verify functionality.
4. **Packaging**: Creates packages for different distributions.
5. **Deployment**: Deploys the packages to GitHub Releases or other artifact repositories.

## Contributing

Contributions are welcome! Please open issues or submit pull requests on GitHub.

### Adding New NIPs

To add a new NIP:

1. Create a new folder in the `nips` directory.
2. Implement the required functionality in C.
3. Update the headers and add test cases.
4. Ensure all tests pass and submit a pull request.

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

```

This README provides comprehensive documentation, including installation instructions, usage examples, details on NIP implementations, GNOME integration, and CI/CD pipeline setup.