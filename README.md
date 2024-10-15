# Nostr C Library

The Nostr C library provides an implementation of the Nostr protocol, including various NIPs (Nostr Improvement Proposals). This library aims to be highly portable, suitable for use in IoT environments, and provides bindings for integration with the GNOME desktop environment.

## Features

- Nostr event handling
- JSON (de)serialization with optional NSON support
- NIP implementations (e.g., NIP-04, NIP-05, NIP-13, NIP-19, NIP-29, NIP-31, NIP-34)
- Optional memory management handled by the library

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

### Adding New NIPs

To add a new NIP:

1. Create a new folder in the `nips` directory.
2. Implement the required functionality in C.
3. Update the headers and add test cases.
4. Ensure all tests pass and submit a pull request.

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.
