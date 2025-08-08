# Deployment

This project produces C libraries intended to be embedded by other applications. Deployment focuses on packaging, installation, and CI/CD.

## Build Types

- Debug: `-DCMAKE_BUILD_TYPE=Debug` with extra warnings, optional sanitizers.
- Release: `-DCMAKE_BUILD_TYPE=Release` with optimizations. Consider LTO (`-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON`).

## Installation

```sh
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j
sudo make install
```

Installation locations depend on your CMake prefix (`CMAKE_INSTALL_PREFIX`). Use `DESTDIR` for packaging.

## Packaging

- Linux: generate `.deb`/`.rpm` via `cpack` or distro-specific tooling.
- macOS: provide Homebrew formula or install via `make install`.
- Headers are under each module's `include/`; ensure they are installed to `${CMAKE_INSTALL_PREFIX}/include` along with libs in `${CMAKE_INSTALL_PREFIX}/lib`.

## CI/CD

- Recommended steps:
  - Configure: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
  - Build: `cmake --build build -j`
  - Test: `ctest --test-dir build --output-on-failure`
  - Package (optional): `cpack -G TGZ -B build/packages`

- Matrix: Linux (gcc/clang), macOS (clang). Cache dependencies where possible.

## Environment Configurations

- OpenSSL path configuration (macOS): set `OPENSSL_ROOT_DIR`, `OPENSSL_INCLUDE_DIR`, `OPENSSL_LIBRARIES`.
- `PKG_CONFIG_PATH` must include `libsecp256k1` (and optionally `jansson`) `.pc` files if not in default locations.

## Monitoring & Logging

- As a library, logging is up to the embedding application. Expose hooks or callbacks for log sinks if needed.
- For CI, capture test logs and artifacts. Fail the pipeline on any sanitizer errors.
