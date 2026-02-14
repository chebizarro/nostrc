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

# ── Find dependencies ────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4     REQUIRED IMPORTED_TARGET gtk4>=4.6)
pkg_check_modules(ADW      REQUIRED IMPORTED_TARGET libadwaita-1>=1.2)
pkg_check_modules(GLIB2    REQUIRED IMPORTED_TARGET glib-2.0>=2.70)
pkg_check_modules(GOBJECT2 REQUIRED IMPORTED_TARGET gobject-2.0>=2.70)
pkg_check_modules(GIO2     REQUIRED IMPORTED_TARGET gio-2.0>=2.70)

# ── Version header (configure from template) ─────────────────────────
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/nostr-gtk-1.0/nostr-gtk-version.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/nostr-gtk-version.h
  @ONLY)

# ── GResource compilation ────────────────────────────────────────────
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)

set(GRESOURCE_XML ${CMAKE_CURRENT_SOURCE_DIR}/data/nostr-gtk.gresource.xml)
set(GRESOURCE_C   ${CMAKE_CURRENT_BINARY_DIR}/nostr-gtk-resources.c)

add_custom_command(
  OUTPUT ${GRESOURCE_C}
  COMMAND ${GLIB_COMPILE_RESOURCES}
          --generate-source
          --sourcedir=${CMAKE_CURRENT_SOURCE_DIR}/data
          --target=${GRESOURCE_C}
          ${GRESOURCE_XML}
  DEPENDS ${GRESOURCE_XML}
  COMMENT "Compiling nostr-gtk GResources"
  VERBATIM)

# ── Source files ─────────────────────────────────────────────────────
set(NOSTR_GTK_SOURCES
  src/nostr-gtk-init.c
  ${GRESOURCE_C}
)

set(NOSTR_GTK_HEADERS
  include/nostr-gtk-1.0/nostr-gtk.h
)

# ── Library target ───────────────────────────────────────────────────
add_library(nostr_gtk ${NOSTR_GTK_SOURCES})

set_target_properties(nostr_gtk PROPERTIES
  OUTPUT_NAME nostr-gtk-1.0
  VERSION     ${NOSTR_GTK_VERSION}
  SOVERSION   ${NOSTR_GTK_MAJOR_VERSION}
)

target_include_directories(nostr_gtk
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/nostr-gtk-1.0
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
)

# Inherit sanitizer flags from root CMakeLists.txt
apply_sanitizers(nostr_gtk)

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
          "-Wl,-force_load,$<TARGET_FILE:nostr_gtk>")
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
          --output ${_gir_file}
          ${_gir_input}
      COMMAND ${G_IR_COMPILER} ${_gir_file} -o ${_typelib}
      DEPENDS ${_gir_lib_dep}
      COMMENT "Generating GNostrGtk-1.0 GIR and typelib"
      VERBATIM
    )

    add_custom_target(nostr-gtk-gir ALL DEPENDS ${_typelib})

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
