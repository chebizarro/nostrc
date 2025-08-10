### Developer Notes (libgo)

- Sanitizers (Debug):

```
# AddressSanitizer + UndefinedBehaviorSanitizer
cmake -S libgo -B build -DCMAKE_BUILD_TYPE=Debug -DGO_ENABLE_ASAN=ON -DGO_ENABLE_UBSAN=ON
cmake --build build -j && ctest --test-dir build --output-on-failure

# ThreadSanitizer
cmake -S libgo -B build_tsan -DCMAKE_BUILD_TYPE=Debug -DGO_ENABLE_TSAN=ON
cmake --build build_tsan -j && ctest --test-dir build_tsan --output-on-failure
```

- Warnings:

```
cmake -S libgo -B build -DCMAKE_BUILD_TYPE=Debug -DGO_WARNINGS_AS_ERRORS=ON
```

# Nostr C Library

![libgo CI](https://github.com/chebizarro/nostrc/actions/workflows/libgo-ci.yml/badge.svg)

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

## Documentation

- See `docs/LIBJSON.md` for libjson API, NIP-01 #tag mapping, robustness rules, and tests.
- See `docs/SHUTDOWN.md` for libnostr/libgo shutdown order, invariants, and troubleshooting.

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
#include "nostr-event.h"
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

## Shutdown Quick Reference

For correct relay/connection/subscription teardown and to avoid hangs or use-after-free during shutdown, see:

- docs/SHUTDOWN.md

Key points:

- Cancel context, close relay queues, snapshot+null the connection.
- Wait for relay workers to exit, then free `conn` channels.
- Finally call `connection_close(conn)`; it closes channels but does not free them.

The official Nostr NIPs are vendored as a git submodule under `docs/nips`.
- Update with `scripts/update_nips.sh`
- Keep the submodule pinned; bump deliberately in separate commits
- Code under `nips/nipXX/` MUST reference the matching `docs/nips/XX.md`
