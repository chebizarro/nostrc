# Nostr C Library

The Nostr C library provides an implementation of the Nostr protocol, including various NIPs (Nostr Improvement Proposals). This library aims to be highly portable, suitable for use in IoT environments, and provides bindings for integration with the GNOME desktop environment.

## Features

- Nostr event handling
- JSON (de)serialization with optional NSON support
- NIP implementations (e.g., NIP-04, NIP-05, NIP-13, NIP-19, NIP-29, NIP-31, NIP-34)
- Optional memory management handled by the library

## Quick Start

Build the libraries and tests with CMake:

```sh
git clone https://github.com/chebizarro/nostrc.git
cd nostrc
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
ctest --output-on-failure
```

Install system-wide (optional):

```sh
sudo make install
```

Link in your C project:

```cmake
find_library(NOSTR_LIB libnostr REQUIRED)
find_library(NOSTR_JSON_LIB nostr_json REQUIRED)
find_library(NSYNC_LIB nsync REQUIRED)
find_package(OpenSSL REQUIRED)
pkg_check_modules(SECP256K1 REQUIRED libsecp256k1)

add_executable(my_app main.c)
target_link_libraries(my_app PRIVATE ${NOSTR_LIB} ${NOSTR_JSON_LIB} ${NSYNC_LIB} OpenSSL::SSL OpenSSL::Crypto ${SECP256K1_LIBRARIES})
```

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
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
sudo make install
```

## NIP Implementations

This library includes various Nostr Improvement Proposals (NIPs):

- **NIP-04**: Encrypted Direct Messages
- **NIP-05**: Mapping Nostr keys to DNS-based identifiers
- **NIP-13**: Proof-of-Work
- **NIP-19**: Bech32-encoded entities
- **NIP-29**: Simple group management
- **NIP-31**: Alternative content
- **NIP-34**: GitHub-like repository management on Nostr

## Contributing

Contributions are welcome! Please open issues or submit pull requests on GitHub.

Guidelines:

1. Fork and create a topic branch.
2. Add focused changes with tests in `tests/` or `libgo/tests/`.
3. Update docs (README, ARCHITECTURE, API) when public APIs change.
4. Run `ctest` and ensure no regressions.
5. Follow the style in `CODING_STANDARDS.md`.

### Usage Examples

See `examples/` for basic JSON integration and event serialization. A minimal flow:

```c
#include "event.h"
#include "keys.h"

int main(void) {
    NostrEvent *ev = create_event();
    // set fields on ev...
    char *json = event_serialize(ev);
    // use json...
    free(json);
    free_event(ev);
    return 0;
}
```

### Adding New NIPs

To add a new NIP:

1. Create a new folder in the `nips` directory.
2. Implement the required functionality in C.
3. Update the headers and add test cases.
4. Ensure all tests pass and submit a pull request.

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.
