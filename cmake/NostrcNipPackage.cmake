include_guard(GLOBAL)
include(GNUInstallDirs)
include("${CMAKE_CURRENT_LIST_DIR}/VersionHelpers.cmake")
include(CMakePackageConfigHelpers)
include("${CMAKE_CURRENT_LIST_DIR}/NostrcPkgConfig.cmake")

# Keep pkg-config as the installed dependency authority, as for bud02/blossom.
# The component version must be declared by its owning CMakeLists.txt first.
function(nostrc_install_nip target component requires requires_private)
  cmake_parse_arguments(PARSE_ARGV 4 NIP "" "" "EXTERNAL_PUBLIC;EXTERNAL_PRIVATE")
  if(NIP_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "Unknown nostrc_install_nip arguments: ${NIP_UNPARSED_ARGUMENTS}")
  endif()
  set(NIP_PC_LIBS "")
  set(NIP_PC_LIBS_PRIVATE "")
  set(NIP_PC_EXTERNAL_CFLAGS "")
  if(NIP_EXTERNAL_PUBLIC)
    nostrc_pkg_config_shared(_public ${NIP_EXTERNAL_PUBLIC})
    set(NIP_PC_LIBS "${_public_LIBS}")
    set(NIP_PC_EXTERNAL_CFLAGS "${_public_CFLAGS}")
  endif()
  if(NIP_EXTERNAL_PRIVATE)
    nostrc_pkg_config_shared(_private ${NIP_EXTERNAL_PRIVATE})
    set(NIP_PC_LIBS_PRIVATE "${_private_LIBS}")
  endif()
  set(_version "${${component}_VERSION}")
  set(${target}_VERSION "${_version}")
  string(REGEX REPLACE "-glib$" "" _namespace "${component}")
  set_target_properties(${target} PROPERTIES
    COMPONENT_VERSION_HEADER "${${component}_VERSION_HEADER}"
    COMPONENT_VERSION_HEADER_DIR "${${component}_VERSION_HEADER_DIR}"
    COMPONENT_VERSION_HEADER_INSTALL_DIR "${CMAKE_INSTALL_INCLUDEDIR}/nostr/${_namespace}")
  apply_versioning(${target})
  get_target_property(_library ${target} OUTPUT_NAME)
  if(NOT _library)
    set(_library "${target}")
  endif()
  set(NIP_PC_NAME "${component}")
  set(NIP_PC_VERSION "${_version}")
  set(NIP_PC_LIBRARY "${_library}")
  set(NIP_PC_REQUIRES "${requires}")
  set(NIP_PC_REQUIRES_PRIVATE "${requires_private}")
  set(NIP_PC_CFLAGS "")
  # Windows static consumers must not see dllimport annotations.
  get_target_property(_type ${target} TYPE)
  if(_type STREQUAL "STATIC_LIBRARY")
    get_target_property(_defs ${target} INTERFACE_COMPILE_DEFINITIONS)
    foreach(_def IN LISTS _defs)
      if(_def MATCHES "^[A-Z0-9_]+_STATIC_DEFINE$")
        string(APPEND NIP_PC_CFLAGS " -D${_def}")
      endif()
    endforeach()
  endif()
  configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/nip.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/${component}.pc" @ONLY)
  install(TARGETS ${target}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${component}.pc"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

  # An aggregate dispatcher has no meaningful aggregate version. Components
  # are versioned by their .pc files; install it even in a NIP-only subset.
  get_property(_config_installed GLOBAL PROPERTY NOSTRC_NIP_CONFIG_INSTALLED)
  if(NOT _config_installed)
    configure_package_config_file(
      "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/nostrcConfig.cmake.in"
      "${CMAKE_CURRENT_BINARY_DIR}/nostrcConfig.cmake"
      INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/nostrc")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/nostrcConfig.cmake"
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nostrc)
    set_property(GLOBAL PROPERTY NOSTRC_NIP_CONFIG_INSTALLED TRUE)
  endif()
endfunction()
