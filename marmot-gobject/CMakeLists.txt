cmake_minimum_required(VERSION 3.22)
project(marmot-gobject C)

set(MARMOT_GOBJECT_MAJOR_VERSION 1)
set(MARMOT_GOBJECT_MINOR_VERSION 0)
set(MARMOT_GOBJECT_MICRO_VERSION 0)
set(MARMOT_GOBJECT_VERSION "${MARMOT_GOBJECT_MAJOR_VERSION}.${MARMOT_GOBJECT_MINOR_VERSION}.${MARMOT_GOBJECT_MICRO_VERSION}")

# Generate version header from template
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/marmot-gobject-1.0/marmot-gobject-version.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/marmot-gobject-1.0/marmot-gobject-version.h
  @ONLY
)

# ─── Dependencies ─────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0>=2.70)
pkg_check_modules(GOBJECT REQUIRED IMPORTED_TARGET gobject-2.0>=2.70)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0>=2.70)

# libmarmot — either via sibling target or pkg-config
if(NOT TARGET marmot)
  pkg_check_modules(MARMOT REQUIRED IMPORTED_TARGET marmot)
endif()

# ─── Source files ─────────────────────────────────────────────────────────
set(MARMOT_GOBJECT_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/marmot-gobject-enums.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/marmot-gobject-group.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/marmot-gobject-message.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/marmot-gobject-welcome.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/marmot-gobject-storage.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/marmot-gobject-client.c
)

set(MARMOT_GOBJECT_PUBLIC_HEADERS
  ${CMAKE_CURRENT_SOURCE_DIR}/include/marmot-gobject-1.0/marmot-gobject.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/marmot-gobject-1.0/marmot-gobject-enums.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/marmot-gobject-1.0/marmot-gobject-client.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/marmot-gobject-1.0/marmot-gobject-group.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/marmot-gobject-1.0/marmot-gobject-message.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/marmot-gobject-1.0/marmot-gobject-welcome.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/marmot-gobject-1.0/marmot-gobject-storage.h
  ${CMAKE_CURRENT_BINARY_DIR}/include/marmot-gobject-1.0/marmot-gobject-version.h
)

# ─── Library target ──────────────────────────────────────────────────────
add_library(marmot-gobject ${MARMOT_GOBJECT_SOURCES})
set_target_properties(marmot-gobject PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  OUTPUT_NAME marmot-gobject-1.0
  VERSION ${MARMOT_GOBJECT_VERSION}
  SOVERSION ${MARMOT_GOBJECT_MAJOR_VERSION}
)

target_include_directories(marmot-gobject
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Link GLib/GObject/GIO publicly (consumers need them)
target_link_libraries(marmot-gobject
  PUBLIC
    PkgConfig::GLIB
    PkgConfig::GOBJECT
    PkgConfig::GIO
)

# Link libmarmot — either as sibling target or pkg-config
if(TARGET marmot)
  target_link_libraries(marmot-gobject PUBLIC marmot)
else()
  target_link_libraries(marmot-gobject PUBLIC PkgConfig::MARMOT)
endif()

# Warn on everything for development
target_compile_options(marmot-gobject PRIVATE
  $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Wno-unused-parameter>
)

# ─── pkg-config ──────────────────────────────────────────────────────────
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/marmot-gobject.pc.in
  ${CMAKE_CURRENT_BINARY_DIR}/marmot-gobject-1.0.pc
  @ONLY
)

# ─── GObject Introspection (GIR) ────────────────────────────────────────
# GIR generation via g-ir-scanner is primarily supported via Meson.
# For CMake, we provide a custom command when g-ir-scanner is available.
option(MARMOT_GOBJECT_INTROSPECTION "Generate GObject Introspection data" OFF)

if(MARMOT_GOBJECT_INTROSPECTION)
  find_program(GIR_SCANNER g-ir-scanner)
  find_program(GIR_COMPILER g-ir-compiler)

  if(GIR_SCANNER AND GIR_COMPILER)
    set(GIR_NAMESPACE "Marmot")
    set(GIR_NSVERSION "1.0")
    set(GIR_FILE "${GIR_NAMESPACE}-${GIR_NSVERSION}.gir")
    set(TYPELIB_FILE "${GIR_NAMESPACE}-${GIR_NSVERSION}.typelib")

    # Collect all header file paths as strings for the scanner
    set(GIR_HEADER_ARGS "")
    foreach(h ${MARMOT_GOBJECT_PUBLIC_HEADERS})
      list(APPEND GIR_HEADER_ARGS "${h}")
    endforeach()

    set(GIR_SOURCE_ARGS "")
    foreach(s ${MARMOT_GOBJECT_SOURCES})
      list(APPEND GIR_SOURCE_ARGS "${s}")
    endforeach()

    add_custom_command(
      OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${GIR_FILE}
      COMMAND ${GIR_SCANNER}
        --namespace=${GIR_NAMESPACE}
        --nsversion=${GIR_NSVERSION}
        --symbol-prefix=marmot_gobject
        --identifier-prefix=MarmotGobject
        --library=marmot-gobject-1.0
        --include=GLib-2.0
        --include=GObject-2.0
        --include=Gio-2.0
        --pkg=glib-2.0
        --pkg=gobject-2.0
        --pkg=gio-2.0
        --warn-all
        --no-libtool
        -I${CMAKE_CURRENT_SOURCE_DIR}/include
        -I${CMAKE_CURRENT_BINARY_DIR}/include
        -L$<TARGET_FILE_DIR:marmot-gobject>
        --output=${CMAKE_CURRENT_BINARY_DIR}/${GIR_FILE}
        ${GIR_HEADER_ARGS}
        ${GIR_SOURCE_ARGS}
      DEPENDS marmot-gobject ${MARMOT_GOBJECT_PUBLIC_HEADERS} ${MARMOT_GOBJECT_SOURCES}
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Generating ${GIR_FILE}"
    )

    add_custom_command(
      OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${TYPELIB_FILE}
      COMMAND ${GIR_COMPILER}
        --output=${CMAKE_CURRENT_BINARY_DIR}/${TYPELIB_FILE}
        ${CMAKE_CURRENT_BINARY_DIR}/${GIR_FILE}
      DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${GIR_FILE}
      COMMENT "Compiling ${TYPELIB_FILE}"
    )

    add_custom_target(marmot-gobject-gir ALL
      DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${GIR_FILE}
              ${CMAKE_CURRENT_BINARY_DIR}/${TYPELIB_FILE}
    )

    # Install GIR and typelib
    pkg_get_variable(GIR_INSTALL_DIR gobject-introspection-1.0 girdir)
    pkg_get_variable(TYPELIB_INSTALL_DIR gobject-introspection-1.0 typelibdir)

    if(GIR_INSTALL_DIR)
      install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${GIR_FILE}
        DESTINATION ${GIR_INSTALL_DIR})
    endif()
    if(TYPELIB_INSTALL_DIR)
      install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${TYPELIB_FILE}
        DESTINATION ${TYPELIB_INSTALL_DIR})
    endif()
  else()
    message(WARNING "GIR scanner/compiler not found — introspection disabled")
  endif()
endif()

# ─── Install ─────────────────────────────────────────────────────────────
include(GNUInstallDirs)

install(TARGETS marmot-gobject
  EXPORT marmot-gobject-targets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES ${MARMOT_GOBJECT_PUBLIC_HEADERS}
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/marmot-gobject-1.0
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/marmot-gobject-1.0.pc
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

# Commented out to allow building without export dependency issues
# install(EXPORT marmot-gobject-targets
#   FILE marmot-gobject-targets.cmake
#   NAMESPACE MarmotGobject::
#   DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/marmot-gobject
# )

# ─── Tests ───────────────────────────────────────────────────────────────
if(BUILD_TESTING)
  enable_testing()

  add_executable(test_marmot_gobject
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_marmot_gobject.c
  )
  target_link_libraries(test_marmot_gobject PRIVATE marmot-gobject)
  target_include_directories(test_marmot_gobject PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  )
  add_test(NAME marmot_gobject_test COMMAND test_marmot_gobject)

  # Set test environment for GLib debugging
  set_tests_properties(marmot_gobject_test PROPERTIES
    ENVIRONMENT "G_DEBUG=fatal-warnings,gc-friendly;G_SLICE=always-malloc"
  )
endif()
