# Development Guide

## Local Setup

- Requirements:
  - CMake >= 3.10
  - C compiler (GCC/Clang)
  - OpenSSL (libssl, libcrypto)
  - libsecp256k1 (with pkg-config if possible)
  - jansson (libjansson)
  - nsync
  - Optional: libwebsockets (if enabling relay over websockets)

On macOS (Homebrew):

```sh
brew install cmake openssl jansson nsync pkg-config
# libsecp256k1 may be in brews or build from source
brew install libsecp256k1 || true
```

## Build

```sh
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j
```

Install:

```sh
sudo make install
```

## Running Tests

From `build/`:

```sh
ctest --output-on-failure
```

Tests compile executables defined in `tests/CMakeLists.txt` and in `libgo/`.

## Debugging Tips

- Enable AddressSanitizer for debugging memory issues:

```sh
cmake .. -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
make -j
```

- Verbose build: `make VERBOSE=1`.
- Use `valgrind` (Linux) or `leaks` (macOS) for leak checks.

## Environment Configuration

- Some tests/examples may require environment variables (e.g., paths to test fixtures). Document them in each test file header if added.
- For OpenSSL on macOS:

```sh
export OPENSSL_ROOT_DIR=$(brew --prefix openssl)
export OPENSSL_LIBRARIES=$(brew --prefix openssl)/lib
export OPENSSL_INCLUDE_DIR=$(brew --prefix openssl)/include
```

## Build & Deployment Process

- CI should run `cmake`, `make -j`, and `ctest` on Linux/macOS.
- Release builds: `-DCMAKE_BUILD_TYPE=Release` and consider `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` for LTO if toolchain supports.

## Troubleshooting

- OpenSSL not found: set `OPENSSL_ROOT_DIR`, `OPENSSL_INCLUDE_DIR`, and `OPENSSL_LIBRARIES`.
- libsecp256k1 not found: ensure pkg-config is installed and `libsecp256k1.pc` is in `PKG_CONFIG_PATH`.
- jansson not found: install library and headers; ensure `pkg-config --libs jansson` works or headers are discoverable.
- nsync not found: install library and headers.

## Maintenance

- Update docs when:
  - Public headers change (`include/*.h`)
  - New NIPs or modules are added under `nips/`
  - Build flags or dependencies change
- Keep CHANGELOG updated for API changes.
