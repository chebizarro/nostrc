#!/bin/bash
set -euo pipefail

# Keep the hand-maintained dependency order, defaults, and special options in
# one canonical template. Directory scanning used to discard all three and
# could silently disable required codecs on regeneration.
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cp "$ROOT/nips/NipOptions.cmake.in" "$ROOT/NipOptions.cmake"

# Retain the existing scaffold operation for new module directories. It never
# replaces a module's authored CMakeLists.txt or invents an enablement policy.
CMAKE_TEMPLATE='
project(%NIP_NAME%)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/NostrcVisibility.cmake")
file(GLOB SOURCES "src/*.c")
add_library(%NIP_NAME% ${SOURCES})
nostrc_library_visibility(%NIP_NAME% NOSTR_%UPPER_NIP_NAME%_API
    nostr/%NIP_NAMESPACE%/export.h "${CMAKE_INSTALL_INCLUDEDIR}/nostr/%NIP_NAMESPACE%")
target_include_directories(%NIP_NAME% PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(%NIP_NAME% PUBLIC nostr)
install(TARGETS %NIP_NAME% DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_%NIP_NAME% ${TEST_SOURCES})
    target_link_libraries(test_%NIP_NAME% %NIP_NAME% nostr)
    add_test(NAME test_%NIP_NAME% COMMAND test_%NIP_NAME%)
endif()
'
for dir in "$ROOT"/nips/*; do
    [ -d "$dir" ] || continue
    [ ! -f "$dir/CMakeLists.txt" ] || continue
    NIP_NAME=$(basename "$dir")
    NIP_NAMESPACE=${NIP_NAME//-/_}
    UPPER_NIP_NAME=$(printf '%s' "$NIP_NAMESPACE" | tr '[:lower:]' '[:upper:]')
    printf '%s\n' "$CMAKE_TEMPLATE" | sed -e "s/%NIP_NAME%/$NIP_NAME/g" \
        -e "s/%NIP_NAMESPACE%/$NIP_NAMESPACE/g" \
        -e "s/%UPPER_NIP_NAME%/$UPPER_NIP_NAME/g" > "$dir/CMakeLists.txt"
done
