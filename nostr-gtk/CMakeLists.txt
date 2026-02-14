# nostr-gtk/CMakeLists.txt — CMake build for the nostr-gtk-1.0 widget library
#
# Builds libnostr-gtk-1.0 (GTK4/libadwaita widgets on top of nostr-gobject).
# Optionally generates GIR/typelib, installs headers, pkg-config, and GResource.
#
# SPDX-License-Identifier: GPL-3.0-or-later

include(GNUInstallDirs)

# ── Version metadata ─────────────────────────────────────────────────
set(NOSTR_GTK_MAJOR_VERSION 1)
set(NOSTR_GTK_MINOR_VERSION 0)
set(NOSTR_GTK_MICRO_VERSION 0)
set(NOSTR_GTK_VERSION "${NOSTR_GTK_MAJOR_VERSION}.${NOSTR_GTK_MINOR_VERSION}.${NOSTR_GTK_MICRO_VERSION}")

# ── Project root ─────────────────────────────────────────────────────
get_filename_component(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# ── Find dependencies ────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4     REQUIRED IMPORTED_TARGET gtk4>=4.6)
pkg_check_modules(ADW      REQUIRED IMPORTED_TARGET libadwaita-1>=1.2)
pkg_check_modules(GLIB2    REQUIRED IMPORTED_TARGET glib-2.0>=2.70)
pkg_check_modules(GOBJECT2 REQUIRED IMPORTED_TARGET gobject-2.0>=2.70)
pkg_check_modules(GIO2     REQUIRED IMPORTED_TARGET gio-2.0>=2.70)
pkg_check_modules(JSON_GLIB REQUIRED IMPORTED_TARGET json-glib-1.0)
pkg_check_modules(SOUP3    QUIET    IMPORTED_TARGET libsoup-3.0)

# ── Version header (configure from template) ─────────────────────────
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/nostr-gtk-1.0/nostr-gtk-version.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/nostr-gtk-version.h
  @ONLY)

# ── Blueprint compilation ────────────────────────────────────────────
find_program(BLUEPRINT_COMPILER blueprint-compiler)

set(NOSTR_GTK_BLUEPRINT_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/nostr-note-card-row.blp
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/gnostr-composer.blp
)

set(NOSTR_GTK_COMPILED_UI_FILES)
if(BLUEPRINT_COMPILER)
  foreach(BLP_FILE ${NOSTR_GTK_BLUEPRINT_SOURCES})
    file(RELATIVE_PATH BLP_REL ${CMAKE_CURRENT_SOURCE_DIR}/data ${BLP_FILE})
    string(REGEX REPLACE "\\.blp$" ".ui" UI_REL ${BLP_REL})
    set(UI_FILE ${CMAKE_CURRENT_SOURCE_DIR}/data/${UI_REL})

    add_custom_command(
      OUTPUT ${UI_FILE}
      COMMAND ${BLUEPRINT_COMPILER} compile --output ${UI_FILE} ${BLP_FILE}
      DEPENDS ${BLP_FILE}
      COMMENT "Compiling nostr-gtk Blueprint: ${BLP_REL}"
      VERBATIM)
    list(APPEND NOSTR_GTK_COMPILED_UI_FILES ${UI_FILE})
  endforeach()

  add_custom_target(nostr_gtk_blueprints ALL DEPENDS ${NOSTR_GTK_COMPILED_UI_FILES})
endif()

# ── GResource compilation ────────────────────────────────────────────
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)

set(GRESOURCE_XML ${CMAKE_CURRENT_SOURCE_DIR}/data/nostr-gtk.gresource.xml)
set(GRESOURCE_C   ${CMAKE_CURRENT_BINARY_DIR}/nostr-gtk-resources.c)

# Collect all UI template files as dependencies
file(GLOB _UI_FILES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/data/ui/*.ui)

add_custom_command(
  OUTPUT ${GRESOURCE_C}
  COMMAND ${GLIB_COMPILE_RESOURCES}
          --generate-source
          --sourcedir=${CMAKE_CURRENT_SOURCE_DIR}/data
          --target=${GRESOURCE_C}
          ${GRESOURCE_XML}
  DEPENDS ${GRESOURCE_XML} ${_UI_FILES}
  COMMENT "Compiling nostr-gtk GResources"
  VERBATIM)

# ── Source files ─────────────────────────────────────────────────────
set(NOSTR_GTK_SOURCES
  src/nostr-gtk-init.c
  src/gnostr-thread-view.c
  src/gnostr-composer.c
  src/gnostr-profile-pane.c
  src/nostr-note-card-row.c
  src/gn-timeline-tabs.c
  src/gnostr-timeline-view.c
  ${GRESOURCE_C}
)

set(NOSTR_GTK_HEADERS
  include/nostr-gtk-1.0/nostr-gtk.h
  include/nostr-gtk-1.0/gnostr-thread-view.h
  include/nostr-gtk-1.0/gnostr-composer.h
  include/nostr-gtk-1.0/gnostr-profile-pane.h
  include/nostr-gtk-1.0/nostr-note-card-row.h
  include/nostr-gtk-1.0/gn-timeline-tabs.h
  include/nostr-gtk-1.0/gnostr-timeline-view.h
)

# ── Library target ───────────────────────────────────────────────────
add_library(nostr_gtk ${NOSTR_GTK_SOURCES})

set_target_properties(nostr_gtk PROPERTIES
  OUTPUT_NAME nostr-gtk-1.0
  VERSION     ${NOSTR_GTK_VERSION}
  SOVERSION   ${NOSTR_GTK_MAJOR_VERSION}
)

# Project root for resolving app-layer include paths (transitional)
get_filename_component(_NOSTR_GTK_PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

target_include_directories(nostr_gtk
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/nostr-gtk-1.0
    # Transitional: app-specific headers not yet moved to nostr-gobject/nostr-gtk
    ${CMAKE_SOURCE_DIR}/apps/gnostr/src
    ${CMAKE_SOURCE_DIR}/apps/gnostr/src/ui
    ${CMAKE_SOURCE_DIR}/apps/gnostr/src/model
    ${CMAKE_SOURCE_DIR}/apps/gnostr/src/util
    ${CMAKE_SOURCE_DIR}/libnostr/include
    ${CMAKE_SOURCE_DIR}/libgo/include
    # nostrdb headers (required by storage_ndb in profile pane)
    ${CMAKE_SOURCE_DIR}/third_party/nostrdb/src
    ${CMAKE_SOURCE_DIR}/third_party/nostrdb/ccan
    # NIP implementations used by profile pane
    ${CMAKE_SOURCE_DIR}/nips/nip19/include
    ${CMAKE_SOURCE_DIR}/nips/nip10/include
    ${CMAKE_SOURCE_DIR}/nips/nip25/include
    # Generated config header (from libnostr)
    ${CMAKE_BINARY_DIR}/libnostr/generated
)

# Public dependencies (propagated to consumers)
target_link_libraries(nostr_gtk
  PUBLIC
    PkgConfig::GTK4
    PkgConfig::ADW
    PkgConfig::GLIB2
    PkgConfig::GOBJECT2
    PkgConfig::GIO2
)

# Private dependencies (implementation only)
target_link_libraries(nostr_gtk
  PRIVATE
    nostr_gobject
    PkgConfig::JSON_GLIB
)

# Optional: libsoup for async image loading
if(SOUP3_FOUND)
  target_link_libraries(nostr_gtk PRIVATE PkgConfig::SOUP3)
  target_compile_definitions(nostr_gtk PRIVATE HAVE_SOUP3=1)
endif()

# Inherit sanitizer flags from root CMakeLists.txt
apply_sanitizers(nostr_gtk)

# Ensure blueprints are compiled before the library
if(BLUEPRINT_COMPILER)
  add_dependencies(nostr_gtk nostr_gtk_blueprints)
endif()

# ── GObject Introspection ────────────────────────────────────────────
option(NOSTR_GTK_ENABLE_GIR "Generate GNostrGtk-1.0 GIR and typelib" OFF)

if(NOSTR_GTK_ENABLE_GIR)
  find_program(G_IR_SCANNER  g-ir-scanner)
  find_program(G_IR_COMPILER g-ir-compiler)

  if(G_IR_SCANNER AND G_IR_COMPILER)
    get_target_property(_gtk_type nostr_gtk TYPE)
    if(_gtk_type STREQUAL "STATIC_LIBRARY")
      set(_gir_wrapper_dir "${CMAKE_CURRENT_BINARY_DIR}/gir_wrappers")
      file(MAKE_DIRECTORY ${_gir_wrapper_dir})
      file(WRITE "${_gir_wrapper_dir}/dummy.c"
           "/* GIR shared wrapper */\nint _nostr_gtk_gir_dummy(void){return 0;}\n")
      add_library(nostr_gtk_shared SHARED "${_gir_wrapper_dir}/dummy.c")
      target_link_libraries(nostr_gtk_shared PRIVATE nostr_gtk)
      if(APPLE)
        target_link_options(nostr_gtk_shared PRIVATE
          "-Wl,-force_load,$<TARGET_FILE:nostr_gtk>"
          "-Wl,-undefined,dynamic_lookup")
      else()
        target_link_options(nostr_gtk_shared PRIVATE
          -Wl,--whole-archive $<TARGET_FILE:nostr_gtk> -Wl,--no-whole-archive)
      endif()
      set(_gir_lib_name nostr_gtk_shared)
      set(_gir_lib_dep  nostr_gtk_shared)
    else()
      set(_gir_lib_name nostr_gtk)
      set(_gir_lib_dep  nostr_gtk)
    endif()

    set(_gir_input "")
    foreach(_hdr ${NOSTR_GTK_HEADERS})
      list(APPEND _gir_input "${CMAKE_CURRENT_SOURCE_DIR}/${_hdr}")
    endforeach()
    foreach(_src ${NOSTR_GTK_SOURCES})
      # Skip generated files (absolute paths)
      if(NOT IS_ABSOLUTE "${_src}")
        list(APPEND _gir_input "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
      endif()
    endforeach()

    set(_gir_dir  "${CMAKE_CURRENT_BINARY_DIR}/gir")
    set(_gir_file "${_gir_dir}/GNostrGtk-1.0.gir")
    set(_typelib  "${_gir_dir}/GNostrGtk-1.0.typelib")

    add_custom_command(
      OUTPUT ${_gir_file} ${_typelib}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${_gir_dir}
      COMMAND ${CMAKE_COMMAND} -E env
        GI_SCANNER_DISABLE_CACHE=1
        "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:${_gir_lib_dep}>:$ENV{LD_LIBRARY_PATH}"
        "DYLD_FALLBACK_LIBRARY_PATH=$<TARGET_FILE_DIR:${_gir_lib_dep}>:$ENV{DYLD_FALLBACK_LIBRARY_PATH}"
        ${G_IR_SCANNER}
          --namespace=GNostrGtk
          --nsversion=1.0
          --symbol-prefix=nostr_gtk
          --identifier-prefix=NostrGtk
          --warn-all
          --library ${_gir_lib_name}
          --library-path $<TARGET_FILE_DIR:${_gir_lib_dep}>
          --pkg glib-2.0 --pkg gobject-2.0 --pkg gio-2.0
          --pkg gtk4 --pkg libadwaita-1
          -I ${CMAKE_CURRENT_SOURCE_DIR}/include/nostr-gtk-1.0
          -I ${CMAKE_CURRENT_BINARY_DIR}
          --include=GLib-2.0
          --include=GObject-2.0
          --include=Gio-2.0
          --include=Gtk-4.0
          --include=Adw-1
          --include=GNostr-1.0
          --add-include-path ${CMAKE_BINARY_DIR}/nostr-gobject/gir
          --output ${_gir_file}
          ${_gir_input}
      COMMAND ${G_IR_COMPILER}
          --includedir ${CMAKE_BINARY_DIR}/nostr-gobject/gir
          ${_gir_file} -o ${_typelib}
      DEPENDS ${_gir_lib_dep}
      COMMENT "Generating GNostrGtk-1.0 GIR and typelib"
      VERBATIM
    )

    add_custom_target(nostr-gtk-gir ALL DEPENDS ${_typelib})

    # Ensure nostr-gobject GIR is built first if it exists (GNostrGtk depends on GNostr-1.0)
    if(TARGET nostr-gobject-gir)
      add_dependencies(nostr-gtk-gir nostr-gobject-gir)
    endif()

    install(FILES ${_gir_file}
            DESTINATION ${CMAKE_INSTALL_DATADIR}/gir-1.0)
    install(FILES ${_typelib}
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/girepository-1.0)
  else()
    message(WARNING "g-ir-scanner/g-ir-compiler not found; GIR generation disabled")
  endif()
endif()

# ── pkg-config ───────────────────────────────────────────────────────
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/nostr-gtk-1.0.pc"
"prefix=${CMAKE_INSTALL_PREFIX}
exec_prefix=\${prefix}
libdir=\${exec_prefix}/${CMAKE_INSTALL_LIBDIR}
includedir=\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}

Name: nostr-gtk-1.0
Description: GTK4/libadwaita widgets for Nostr — built on nostr-gobject
Version: ${NOSTR_GTK_VERSION}
Requires: glib-2.0 gobject-2.0 gio-2.0 gtk4 libadwaita-1
Requires.private: nostr-gobject-1.0
Libs: -L\${libdir} -lnostr-gtk-1.0
Cflags: -I\${includedir}/nostr-gtk-1.0
")

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/nostr-gtk-1.0.pc"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# ── Installation ─────────────────────────────────────────────────────
install(TARGETS nostr_gtk
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES ${NOSTR_GTK_HEADERS}
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nostr-gtk-1.0)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/nostr-gtk-version.h"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nostr-gtk-1.0)
