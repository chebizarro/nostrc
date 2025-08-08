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

### Linux

- Ubuntu/Debian:

```sh
sudo apt update
sudo apt install -y build-essential cmake pkg-config git \
  libssl-dev libjansson-dev \
  libsecp256k1-dev || true

# nsync is not packaged on all distros; build from source if not available
# Build nsync from source
git clone https://github.com/google/nsync.git
cd nsync
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
sudo make install
```

- Fedora/RHEL (dnf):

```sh
sudo dnf install -y gcc gcc-c++ cmake make pkgconf-pkg-config git \
  openssl-devel jansson-devel \
  libsecp256k1-devel || true

# Build nsync from source if not available via dnf (same steps as above)
```

- Arch Linux:

```sh
sudo pacman -S --needed base-devel cmake pkgconf git openssl jansson \
  libsecp256k1 || true

# Build nsync from source if not available via pacman
```

Notes:
- If `libsecp256k1-dev`/`-devel` is not available on your distro, build from source:

```sh
git clone https://github.com/bitcoin-core/secp256k1.git
cd secp256k1
./autogen.sh
./configure --enable-module-recovery --enable-experimental --enable-module-ecdh
make -j
sudo make install
```

### Windows

Windows options are varied; we recommend MSYS2 or WSL for a Unix-like environment.

- MSYS2 (native MinGW toolchain):

```sh
# Install MSYS2 from https://www.msys2.org/, then in MSYS2 MinGW64 shell:
pacman -Syu --noconfirm
pacman -S --needed --noconfirm \
  mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-pkgconf git \
  mingw-w64-x86_64-openssl mingw-w64-x86_64-jansson

# libsecp256k1 and nsync may not be available: build both from source
# Build secp256k1 and nsync as in Linux steps, using MinGW Makefiles or CMake
```

- WSL (Windows Subsystem for Linux):

```sh
# In Ubuntu (WSL) follow the Ubuntu/Debian instructions above
```

- vcpkg (optional, MSVC toolchain):
  - Install vcpkg and integrate with Visual Studio.
  - Install available ports: `vcpkg install openssl jansson libwebsockets`.
  - `libsecp256k1` and `nsync` may require building from source; prefer MSYS2/WSL if possible.

### Optional: libwebsockets

If you plan to enable websocket support in `libnostr`:

```sh
# Ubuntu/Debian
sudo apt install -y libwebsockets-dev

# Fedora
sudo dnf install -y libwebsockets-devel

# macOS (Homebrew)
brew install libwebsockets
```

### Common CMake Issues

- Missing `NipOptions.cmake` during configure:
  - Ensure all submodules or auxiliary CMake files are present. Try:

```sh
git submodule update --init --recursive
```

- `libjansson` not found:
  - Install the development package and ensure `pkg-config --libs jansson` works.

- `nsync` not found:
  - Ensure headers and library are installed (e.g., `/usr/local/include/nsync.h`, `/usr/local/lib/libnsync.a` or `.so`).
  - On macOS with Homebrew, it should be auto-discovered after `brew install nsync`.

- `libsecp256k1` not found:
  - Install dev package or build from source with `--enable-module-recovery` as shown above.

After installing dependencies, clear and reconfigure the build:

```sh
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
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
