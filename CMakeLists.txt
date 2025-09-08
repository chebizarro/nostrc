# Feature toggles
option(WITH_NOSTRDB "Build nostrdb storage component" ON)

# Subprojects
if (WITH_NOSTRDB)
  if (EXISTS ${CMAKE_SOURCE_DIR}/components/nostrdb/CMakeLists.txt)
    add_subdirectory(components/nostrdb)
  endif()
endif()

# Under ThreadSanitizer, prefer lock-based channel try paths to reduce races seen by TSAN
if (GO_ENABLE_TSAN)
  add_compile_definitions(NOSTR_CHANNEL_ATOMIC_TRY=0)
endif()

if (EXISTS ${CMAKE_SOURCE_DIR}/apps/relayd/CMakeLists.txt)
  add_subdirectory(apps/relayd)
endif()
# Global hardening flags (GCC/Clang); keep portable and avoid breaking MSVC
if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-fstack-protector-strong -D_FORTIFY_SOURCE=2 -fno-strict-aliasing)
  # Position independent for shared libs; prefer PIE for executables where supported
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  if (UNIX AND NOT APPLE)
    add_compile_options(-fPIE)
    add_link_options(-pie)
    # ELF linker hardening
    add_link_options(-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)
  endif()
endif()
cmake_minimum_required(VERSION 3.30)
project(nostrc)

# Enable CTest early so all subdirectories can register tests (including NIPs)
enable_testing()

# Top-level build for the nostrc monorepo.
# This project builds multiple libraries:
#  - libgo: Go-like concurrency primitives in C (channels, contexts, etc.)
#  - libnostr: Core Nostr protocol types and helpers
#  - libjson: JSON interop layer based on jansson
# It also configures tests under `tests/` via CTest.

# Options to enable/disable NIPs and NSON
# Toggle optional NSON support (custom serialization). When ON, the `nson/` subdir
# may be built and linked by consumers that need it.
option(ENABLE_NSON "Enable NSON compilation" OFF)

# (enable_testing already called at the top)

# Sanitizer helper (Debug builds)
function(apply_sanitizers target)
  # Always honor GO_ENABLE_* toggles regardless of build type so CI (which may
  # use RelWithDebInfo) still links sanitizer DSOs for executables.
  if (GO_ENABLE_TSAN)
    target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=thread)
    if (UNIX AND NOT APPLE)
      target_link_libraries(${target} PRIVATE tsan)
    endif()
  endif()
  if (GO_ENABLE_ASAN)
    target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address)
    if (UNIX AND NOT APPLE)
      target_link_libraries(${target} PRIVATE asan)
    endif()
  endif()
  if (GO_ENABLE_UBSAN)
    target_compile_options(${target} PRIVATE -fsanitize=undefined)
    target_link_options(${target} PRIVATE -fsanitize=undefined)
    if (UNIX AND NOT APPLE)
      target_link_libraries(${target} PRIVATE ubsan)
    endif()
  endif()
endfunction()

# Also apply sanitizer options globally in Debug builds so that all libraries and
# executables are consistently compiled and linked with the sanitizer runtimes.
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  if (GO_ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -O2)
    add_link_options(-fsanitize=thread)
    if (UNIX AND NOT APPLE)
      # Ensure explicit tsan runtime linkage for lld on Linux
      add_link_options(-ltsan)
    endif()
  else()
    if (GO_ENABLE_ASAN)
      add_compile_options(-fsanitize=address -fno-omit-frame-pointer -O2)
      add_link_options(-fsanitize=address)
      # Ensure compile/link driver always carries sanitizer in Debug (optimized)
      set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fsanitize=address -fno-omit-frame-pointer -O2")
      set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address -fno-omit-frame-pointer -O2")
      # Apply to all executables/shared libs as well (AppleClang needs it on the final link)
      set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fsanitize=address")
      set(CMAKE_SHARED_LINKER_FLAGS_DEBUG "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} -fsanitize=address")
      set(CMAKE_MODULE_LINKER_FLAGS_DEBUG "${CMAKE_MODULE_LINKER_FLAGS_DEBUG} -fsanitize=address")
    endif()
    if (GO_ENABLE_UBSAN)
      add_compile_options(-fsanitize=undefined -O2)
      add_link_options(-fsanitize=undefined)
      set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fsanitize=undefined -O2")
      set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=undefined -O2")
      set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fsanitize=undefined")
      set(CMAKE_SHARED_LINKER_FLAGS_DEBUG "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} -fsanitize=undefined")
      set(CMAKE_MODULE_LINKER_FLAGS_DEBUG "${CMAKE_MODULE_LINKER_FLAGS_DEBUG} -fsanitize=undefined")
    endif()
  endif()
  # Ensure executables and shared libs link the sanitizer runtime
  if (GO_ENABLE_TSAN)
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fsanitize=thread")
    set(CMAKE_SHARED_LINKER_FLAGS_DEBUG "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} -fsanitize=thread")
    set(CMAKE_MODULE_LINKER_FLAGS_DEBUG "${CMAKE_MODULE_LINKER_FLAGS_DEBUG} -fsanitize=thread")
    set(CMAKE_C_STANDARD_LIBRARIES "${CMAKE_C_STANDARD_LIBRARIES} -fsanitize=thread")
    # Also set for single-config generators
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=thread")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=thread")
    set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -fsanitize=thread")
  else()
    if (GO_ENABLE_ASAN)
      set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fsanitize=address")
      set(CMAKE_SHARED_LINKER_FLAGS_DEBUG "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} -fsanitize=address")
      set(CMAKE_MODULE_LINKER_FLAGS_DEBUG "${CMAKE_MODULE_LINKER_FLAGS_DEBUG} -fsanitize=address")
      set(CMAKE_C_STANDARD_LIBRARIES "${CMAKE_C_STANDARD_LIBRARIES} -fsanitize=address")
      set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
      set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address")
      set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -fsanitize=address")
    endif()
    if (GO_ENABLE_UBSAN)
      set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fsanitize=undefined")
      set(CMAKE_SHARED_LINKER_FLAGS_DEBUG "${CMAKE_SHARED_LINKER_FLAGS_DEBUG} -fsanitize=undefined")
      set(CMAKE_MODULE_LINKER_FLAGS_DEBUG "${CMAKE_MODULE_LINKER_FLAGS_DEBUG} -fsanitize=undefined")
      set(CMAKE_C_STANDARD_LIBRARIES "${CMAKE_C_STANDARD_LIBRARIES} -fsanitize=undefined")
      set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=undefined")
      set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=undefined")
      set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -fsanitize=undefined")
    endif()
  endif()
endif()

# Build order ensures libgo is available for libnostr/libjson as needed.
add_subdirectory(libgo)
add_subdirectory(libnostr)
add_subdirectory(libjson)


# NIPs are added via NipOptions.cmake per-NIP toggles; avoid adding nips/ twice.
# Include NIP options/subdirs after global sanitizer flags so all NIP targets inherit them.
include(${CMAKE_SOURCE_DIR}/NipOptions.cmake)

# Add test targets from `tests/`.
add_subdirectory(tests)

# GNostr apps (GTK4/GLib/libsecret) — make building apps optional for CI jobs that only need libraries
option(BUILD_APPS "Build application binaries under apps/" ON)
if (BUILD_APPS)
  add_subdirectory(apps)
endif()

# Examples
add_executable(relay_smoke examples/relay_smoke.c)
target_link_libraries(relay_smoke PRIVATE libnostr nostr_json)
find_path(NSYNC_INCLUDE_DIR nsync.h REQUIRED)
target_include_directories(relay_smoke PRIVATE ${NSYNC_INCLUDE_DIR})
find_library(NSYNC_LIB nsync REQUIRED)
target_link_libraries(relay_smoke PRIVATE ${NSYNC_LIB})
apply_sanitizers(relay_smoke)

# NostrdB store demo (optional; requires an installed 'nostrdb' library)
option(ENABLE_NOSTRDB_DEMO "Build ndb_store_demo (requires nostrdb library)" OFF)
if (ENABLE_NOSTRDB_DEMO)
  find_library(NOSTRDB_LIB nostrdb)
  if (NOSTRDB_LIB)
    add_executable(ndb_store_demo examples/ndb_store_demo.c)
    target_link_libraries(ndb_store_demo PRIVATE libnostr nostr_json ${NOSTRDB_LIB})
    target_include_directories(ndb_store_demo PRIVATE ${NSYNC_INCLUDE_DIR})
    target_link_libraries(ndb_store_demo PRIVATE ${NSYNC_LIB})
    apply_sanitizers(ndb_store_demo)
  else()
    message(WARNING "nostrdb library not found; skipping ndb_store_demo. Set ENABLE_NOSTRDB_DEMO=ON and provide NOSTRDB_LIB path to build.")
  endif()
endif()

# --- GNOME integration (GOA provider, Seahorse helpers, optional PKCS#11) ---
# Provide toggles so non-GNOME environments/builders aren't broken by missing deps.
option(GNOSTR_ENABLE_GOA "Build GNOME Online Accounts provider" OFF)
option(GNOSTR_ENABLE_SEAHORSE_HELPERS "Build Seahorse/Secret Service helpers" OFF)
option(GNOSTR_WITH_PKCS11 "Build p11-kit module" OFF)

if (GNOSTR_ENABLE_GOA OR GNOSTR_ENABLE_SEAHORSE_HELPERS OR GNOSTR_WITH_PKCS11)
  find_package(PkgConfig REQUIRED)
endif()

if (GNOSTR_ENABLE_GOA)
  pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
  pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gobject-2.0 gio-2.0)
  pkg_check_modules(GOA REQUIRED IMPORTED_TARGET goa-1.0)
  pkg_check_modules(GOA_BACKEND REQUIRED IMPORTED_TARGET goa-backend-1.0)
  pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)

  # Probe whether the installed goa-backend headers actually expose a complete
  # GoaProviderClass definition (some distros keep it private, making subclassing impossible).
  include(CheckCSourceCompiles)
  set(CMAKE_REQUIRED_INCLUDES
    ${GOA_INCLUDE_DIRS}
    ${GOA_BACKEND_INCLUDE_DIRS}
  )
  set(CMAKE_REQUIRED_DEFINITIONS
    -DGOA_API_IS_SUBJECT_TO_CHANGE=1
    -DGOA_BACKEND_API_IS_SUBJECT_TO_CHANGE=1
  )
  check_c_source_compiles("#include <goa/goa.h>\n#include <goabackend/goabackend.h>\nint main(void){ return (int)sizeof(GoaProviderClass); }" GOA_PROVIDERCLASS_AVAILABLE)
  unset(CMAKE_REQUIRED_INCLUDES)
  unset(CMAKE_REQUIRED_DEFINITIONS)

  if (GOA_PROVIDERCLASS_AVAILABLE)
    add_subdirectory(gnome/goa)
  else()
    message(WARNING "GNOSTR_ENABLE_GOA requested, but goa-backend headers do not expose a complete GoaProviderClass on this system. Disabling GOA provider build.")
    set(GNOSTR_ENABLE_GOA OFF CACHE BOOL "Build GNOME Online Accounts provider" FORCE)
  endif()
  # Install DBus service activation + desktop/appdata if present
  install(FILES gnome/dbus/com.nostr.Signer.service
          DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
  install(FILES data/org.nostr.Signer.desktop
          DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
  install(FILES data/org.nostr.Signer.appdata.xml
          DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
endif()

if (GNOSTR_ENABLE_SEAHORSE_HELPERS)
  pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gobject-2.0 gio-2.0)
  pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
  add_subdirectory(gnome/seahorse)
endif()

if (GNOSTR_WITH_PKCS11)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(P11KIT REQUIRED IMPORTED_TARGET p11-kit-1)
  add_subdirectory(pkcs11)
endif()

## GLib demo: only if libnostr built with GLib support
if (TARGET nostr)
  get_target_property(_nostr_defs nostr COMPILE_DEFINITIONS)
  if (NOSTR_WITH_GLIB)
    # Try to reuse GLib pkg from libnostr configuration
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GLIB QUIET IMPORTED_TARGET glib-2.0>=2.50 gobject-2.0>=2.50)
    if (GLIB_FOUND)
      add_executable(json_provider_demo examples/json_provider_demo.c)
      target_link_libraries(json_provider_demo PRIVATE libnostr nostr_json PkgConfig::GLIB)
      target_compile_definitions(json_provider_demo PRIVATE NOSTR_ENABLE_LEGACY_ALIASES=0)
      target_include_directories(json_provider_demo PRIVATE 
        libnostr/include 
        ${CMAKE_BINARY_DIR}/libnostr/generated)
      # nsync dependency required via libgo headers used transitively
      find_path(NSYNC_INCLUDE_DIR nsync.h REQUIRED)
      target_include_directories(json_provider_demo PRIVATE ${NSYNC_INCLUDE_DIR})
      find_library(NSYNC_LIB nsync REQUIRED)
      target_link_libraries(json_provider_demo PRIVATE ${NSYNC_LIB})
      apply_sanitizers(json_provider_demo)
    endif()
  endif()
endif()


# Optional GObject Introspection generation
option(NOSTR_ENABLE_GI "Enable GIR/typelib generation target" ON)
if (NOSTR_ENABLE_GI)
  find_package(PkgConfig REQUIRED)
  # Ensure our pkg-config files are available at build time by adding build-time pc dirs if needed
  # Consumers typically run this after an install, but for convenience we'll use build artifacts too.
  find_program(G_IR_SCANNER g-ir-scanner)
  find_program(G_IR_COMPILER g-ir-compiler)
  if (G_IR_SCANNER AND G_IR_COMPILER)
    # Includes needed for headers referenced by libnostr public headers
    find_path(NSYNC_INCLUDE_DIR nsync.h)
    add_custom_command(
      OUTPUT ${CMAKE_BINARY_DIR}/gir/Nostr-1.0.typelib
      COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/gir
      # Ensure scanner can resolve libraries at runtime
      COMMAND ${CMAKE_COMMAND} -E env
        GI_SCANNER_DISABLE_CACHE=1
        DYLD_FALLBACK_LIBRARY_PATH=${CMAKE_BINARY_DIR}/libnostr:${CMAKE_BINARY_DIR}/libjson:$ENV{DYLD_FALLBACK_LIBRARY_PATH}
        LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/libnostr:${CMAKE_BINARY_DIR}/libjson:$ENV{LD_LIBRARY_PATH}
        ${G_IR_SCANNER}
        --cflags-begin -std=gnu11 -xc -DNOSTR_WITH_GLIB=1 --cflags-end
        --namespace=Nostr
        --nsversion=1.0
        --symbol-prefix=nostr_
        --identifier-prefix=Nostr
        --warn-all
        --library nostr 
        --extra-library nostr_json 
        --library-path ${CMAKE_BINARY_DIR}/libnostr
        --library-path ${CMAKE_BINARY_DIR}/libjson
        --pkg glib-2.0 --pkg gobject-2.0
        --pkg jansson --pkg libwebsockets --pkg openssl --pkg libsecp256k1
        -I ${CMAKE_SOURCE_DIR}/libnostr/include
        -I ${CMAKE_SOURCE_DIR}/libgo/include
        $<$<BOOL:${NSYNC_INCLUDE_DIR}>:-I${NSYNC_INCLUDE_DIR}>
        -I ${CMAKE_BINARY_DIR}/libnostr/generated
        ${CMAKE_SOURCE_DIR}/libnostr/include/nostr-utils.h
        ${CMAKE_SOURCE_DIR}/libnostr/include/nostr-event.h
        ${CMAKE_SOURCE_DIR}/libnostr/include/nostr-filter.h
        --output ${CMAKE_BINARY_DIR}/gir/Nostr-1.0.gir
      COMMAND ${G_IR_COMPILER} ${CMAKE_BINARY_DIR}/gir/Nostr-1.0.gir -o ${CMAKE_BINARY_DIR}/gir/Nostr-1.0.typelib
      DEPENDS nostr nostr_json
      VERBATIM
    )
  endif()
endif()

if(APPLE)
    add_definitions(-D_DARWIN_C_SOURCE)
else()
    add_definitions(-D_POSIX_C_SOURCE=200809L)
endif()

