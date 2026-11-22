# Installed NIP libraries

Use an installed prefix, not archives from a shared developer build directory.
Packagers should record an exact nostrc commit in their source manifest, check
out that revision and initialize its pinned submodules when enabling nostrdb.
Use a fresh build directory for each revision/configuration. Do not mix static
and shared artifacts or different `NOSTR_WITH_GLIB` ABIs in one prefix.

## Supported subset

The initial-cache file disables unrelated components, including FIPS and
nostrdb, and enables NIP-04/11/17/44/46/51/78/98. NIP-06/19 remain enabled because
the repository always builds the NIP-55L core. System nsync is required by this
packaging recipe. Install the usual development dependencies: C/C++ compilers,
CMake >= 3.22, Ninja, pkg-config, nsync, libwebsockets, OpenSSL, secp256k1,
Jansson, curl, and GLib/GIO/libsecret (the always-built signer core uses these).

```sh
cmake -S . -B _build-package -G Ninja \
  -C tests/packaging/nip-subset.cmake \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DBUILD_SHARED_LIBS=OFF -DNOSTR_WITH_GLIB=OFF -DBUILD_TESTING=OFF
cmake --build _build-package -j4
DESTDIR="$PWD/_stage-package" cmake --install _build-package
```

For shared libnostr/NIP-11/NIP-78/NIP-98 and the optional NIP-46 GLib layer, use a
separate build and prefix with `BUILD_SHARED_LIBS=ON`, `NOSTR_WITH_GLIB=ON`.
NIP-04, NIP-44 core, NIP-46 core and the installed NIP-17/51 wrappers remain
static, matching their existing in-tree ownership policy. NIP-17/51's original
OBJECT targets are unchanged for in-tree callers; their installed archives are
position-independent. The JSON provider (`libnostr-json`) remains shared even
when `BUILD_SHARED_LIBS=OFF`; `pkg-config --static` does not imply a fully static
executable or availability of static system libraries.

With `BUILD_TESTING=ON`, build the selected NIP test targets instead of the
unrestricted `all` target: some unrelated root tests assume disabled NIPs exist.
The packaging recipe above uses `BUILD_TESTING=OFF` and builds the subset in full.

For nostrdb-enabled builds, initialize `third_party/nostrdb` recursively and set
`WITH_NOSTRDB=ON` and `LIBNOSTR_WITH_NOSTRDB=ON`. The installed core closure then
also includes the `nostrdb` archive, libsodium, optional system flatccrt and the
macOS Security framework. FIPS remains independently optional.

`CMAKE_INSTALL_PREFIX` and GNUInstallDirs are configure-time inputs to the .pc
files. Use `DESTDIR` for distro staging; an install-time `--prefix` override does
not rewrite generated metadata. A runnable test prefix should be configured as
that prefix from the start, without `DESTDIR`.

## Layout and discovery

Libraries go to `${CMAKE_INSTALL_LIBDIR}`, .pc files to its `pkgconfig/`, and
headers to `${CMAKE_INSTALL_INCLUDEDIR}`. Include NIP APIs as
`<nostr/nip17/nip17.h>`, `<nostr/nip46/nip46_client.h>`, etc.; NIP-04 uses
`<nostr/nip04.h>`. Generated export/version headers are installed alongside the
APIs. The corrected NIP-46 layout has no extra `nostr/nip46/nostr/nip46` nesting.
Core/libjson headers remain under `include/nostr`, libgo headers under `include`;
use pkg-config Cflags rather than hardcoding these paths.

All NIP .pc files have `Libs: -L${libdir} -l<library>` and
`Cflags: -I${includedir}` plus the applicable static export define. Dependencies:

| Package | Library | Requires | Requires.private |
| --- | --- | --- | --- |
| nip04 | nip04 | nostr >= 2.5.1 | openssl, libsecp256k1 |
| nip11 | nip11 | nostr >= 2.5.1 | libnostr-json, libcurl |
| nip17 | nostr_nip17_core | nostr >= 2.5.1 | nip44 >= 0.1.0, openssl |
| nip44 | nostr_nip44_core | nostr >= 2.5.1 | openssl, libsecp256k1 |
| nip46 | nostr_nip46_core | nostr >= 2.5.1 | nip04 >= 0.1.0, nip44 >= 0.1.0, libnostr-json |
| nip46-glib (when built) | nostr_nip46_glib | nip46, gio-2.0 | — |
| nip51 | nostr_nip51_core | nostr >= 2.5.1 | nip44 >= 0.1.0, jansson |
| nip78 | nostr_nip78_core | nostr >= 2.5.1 | jansson |
| nip98 | nostr_nip98 | nostr >= 2.5.1 | openssl |

These NIP packages need no extra `Libs.private`: their private closure is expressed
through versioned `Requires.private`. `nostr.pc` supplies libnostrgo >= 0.3.1 publicly and
libwebsockets/OpenSSL/secp256k1 privately, plus GLib publicly when enabled and the
optional nostrdb dependencies privately. Its `Libs.private` preserves the resolved
nsync search directory and the optional nostrdb/flatccrt/platform link flags.
`libnostr-json.pc` supplies Jansson and a private dependency on nostr >= 2.5.1.

```sh
export PKG_CONFIG_PATH=/my/prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}
cc consumer.c -o consumer $(pkg-config --cflags --libs --static nip11 nip17 nip46 nip51 nip78 nip98)
```

Always request `--static` when using any of the static-only NIPs (Meson:
`dependency('nip17', static: true)`, etc.). For a shared NIP-11/78/98 consumer use
normal `pkg-config --cflags --libs nip11 nip78 nip98`. For a private non-system prefix,
configure the application's loader RPATH from `pkg-config --variable=libdir
libnostr-json`; do not point it at a developer build tree.

CMake follows the repo's pkg-config-backed aggregate dispatcher:

```cmake
find_package(nostrc REQUIRED COMPONENTS nip11 nip17 nip46 nip51 nip78 nip98)
target_link_libraries(app PRIVATE nostrc::nip11 nostrc::nip17 nostrc::nip46
                                  nostrc::nip51 nostrc::nip78 nostrc::nip98)
```

Set `CMAKE_PREFIX_PATH` and, for nonstandard .pc locations, `PKG_CONFIG_PATH`.
The dispatcher uses the installed static closure, never source/build paths.
Components own their versions, not the aggregate `nostrcConfig.cmake`.

## Version and ABI policy

NIP-04/11/17/44/51/98 start their installed contract at 0.1.0; NIP-46 core/GLib
share 0.2.1. NIP-78 is 0.1.1 for packaging corrections to its initial 0.1.0
packaging contract, with unchanged API and SOVERSION 0. libnostr is 2.5.1, libnostr-json is 2.0.1 and libgo is 0.3.1 for dependency metadata
corrections. `declare_component_version` and `apply_versioning` use the same
policy as libnostr: full SemVer `VERSION`, major `SOVERSION` for shared libraries
(e.g. `libnip11.so.0`, `libnostr_nip98.so.0`; Mach-O `.0.dylib` install names).
Static/OBJECT libraries have version metadata but no SONAME. Follow AGENTS.md's
SemVer policy for future changes; incompatible shared ABIs must also advance the
ABI/SOVERSION rather than silently replacing an incompatible `.so.0`.

A new NIP can reuse `cmake/NostrcNipPackage.cmake`: declare its component version,
install its public headers and call
`nostrc_install_nip(target component "public .pc dependencies" "private .pc dependencies")`.
Add its name to the aggregate dispatcher's component allowlist. Keep the version
in the owning module's CMakeLists.txt and VERSION_MANIFEST.md.

## Installed-consumer regression checks

Configure a writable staging prefix, build its libraries and selected NIP tests
at `-j4`, then run:

```sh
ctest --test-dir _build-package --output-on-failure -j2 \
  -R '^(test_nip(11|17|46.*|51.*|78.*|98|44.*|04.*)|test_(interop|roundtrip))$'
python3 tests/packaging/check-nip-install.py --build _build-package
# For a shared build; add --sanitize for GO_ENABLE_ASAN/UBSAN builds:
python3 tests/packaging/check-nip-install.py --build _build-shared --shared --sanitize
```

The checker installs the build, compiles/runs a tiny consumer outside the source
include layout using only installed pkg-config flags, checks nonempty versions,
and checks installed CMake component discovery. When built, NIP-46 GLib is
also exercised through its installed header and .pc. `--shared` additionally links
NIP-11/78/98 without `--static`. A standalone NIP-78 mode checks its dependency
closure without other NIPs masking missing link metadata. It removes temporary
consumer files afterwards.
CI's core ASan/UBSan job runs this check after building. Match its strict NIP-04
option and sanitizer environment; do not claim hosted Linux/Windows acceptance
from a local macOS run.
