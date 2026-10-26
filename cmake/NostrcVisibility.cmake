include_guard(GLOBAL)
include(GNUInstallDirs)

# Generate the library-owned API annotations. `header` is relative to the
# generated include directory. This can also serve libraries which already own
# their visibility and installation policy.
function(nostrc_generate_export_header target macro header)
  # GenerateExportHeader keeps its template path in directory-scoped variables.
  include(GenerateExportHeader)
  get_target_property(_type ${target} TYPE)
  if(NOT _type MATCHES "^(STATIC|SHARED|MODULE|OBJECT)_LIBRARY$")
    message(FATAL_ERROR "nostrc_generate_export_header requires a library: ${target}")
  endif()
  string(MAKE_C_IDENTIFIER "${target}" _prefix)
  string(TOUPPER "${_prefix}" _prefix)
  set(_static_define "${_prefix}_STATIC_DEFINE")
  set(_include "${CMAKE_CURRENT_BINARY_DIR}/include")
  set(_extra "")
  if(_type STREQUAL "STATIC_LIBRARY" OR _type STREQUAL "OBJECT_LIBRARY")
    # OBJECT libraries have no DLL of their own: they belong to the final
    # consumer, just like archives. Do not import another target's exports.
    target_compile_definitions(${target} PUBLIC ${_static_define})
    # ELF consumers such as libnostr-json resolve public API from the executable's
    # static archive. GenerateExportHeader normally makes static annotations empty;
    # with hidden defaults that would prevent this supported link arrangement.
    set(_extra "
#if !defined(_WIN32) && (defined(__GNUC__) || defined(__clang__))
# undef ${macro}
# define ${macro} __attribute__((visibility(\"default\")))
#endif
")
  endif()
  generate_export_header(${target}
    EXPORT_MACRO_NAME ${macro}
    STATIC_DEFINE ${_static_define}
    EXPORT_FILE_NAME "${_include}/${header}"
    CUSTOM_CONTENT_FROM_VARIABLE _extra)
endfunction()

# Internal archives and modules have no public headers, but still need hidden
# defaults. Loader entry points can opt back in with G_MODULE_EXPORT.
function(nostrc_internal_visibility target)
  get_target_property(_type ${target} TYPE)
  if(NOT _type MATCHES "^(STATIC|SHARED|MODULE|OBJECT)_LIBRARY$")
    message(FATAL_ERROR "nostrc_internal_visibility requires a library: ${target}")
  endif()
  set_target_properties(${target} PROPERTIES
    C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES WINDOWS_EXPORT_ALL_SYMBOLS NO)
  if((_type STREQUAL "SHARED_LIBRARY" OR _type STREQUAL "MODULE_LIBRARY")
      AND UNIX AND NOT APPLE)
    # Do not re-export unannotated symbols from embedded third-party archives.
    # Direct archive consumers still receive their explicit link dependencies.
    target_link_options(${target} PRIVATE "LINKER:--exclude-libs,ALL")
  endif()
endfunction()

# nostrc_library_visibility(target macro header install_directory [NO_INSTALL])
# Keep macro names library-specific: importing one DLL must not export another
# DLL's API. Loader-only modules need an export macro but do not install a header.
function(nostrc_library_visibility target macro header install_directory)
  nostrc_generate_export_header(${target} ${macro} ${header})
  nostrc_internal_visibility(${target})
  set(_include "${CMAKE_CURRENT_BINARY_DIR}/include")
  target_include_directories(${target} PUBLIC
    $<BUILD_INTERFACE:${_include}>)
  # Also cover tests in this directory that deliberately compile library sources.
  set_property(DIRECTORY APPEND PROPERTY INCLUDE_DIRECTORIES "${_include}")
  if(ARGC LESS 5 OR NOT ARGV4 STREQUAL "NO_INSTALL")
    install(FILES "${_include}/${header}" DESTINATION "${install_directory}")
  endif()
endfunction()
