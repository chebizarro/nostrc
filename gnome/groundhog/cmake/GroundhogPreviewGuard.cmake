# The guard of GROUNDHOG_ENCRYPTED_GROUPS_PREVIEW (review M5, nostrc-9xf5):
# the preview turns on encrypted groups before their backfill blockers close
# (src/app/gh-features.h), so it is for development builds only. A release
# build type, CPack, a distribution install prefix or a packaging
# environment is refused at configure time; installing a preview build is
# refused at install time (gnome/groundhog/CMakeLists.txt).
#
# groundhog_preview_problem(<out-var>
#   BUILD_TYPE <CMAKE_BUILD_TYPE> CONFIGURATION_TYPES <CMAKE_CONFIGURATION_TYPES...>
#   INSTALL_PREFIX <CMAKE_INSTALL_PREFIX> [CPACK])
# sets <out-var> to why the preview can't be built here, or "" when it can.
# The environment is read too. Script mode (cmake -P) can call it: see
# tests/check_preview_guard.cmake.

set(GROUNDHOG_PREVIEW_RELEASE_TYPES RELEASE RELWITHDEBINFO MINSIZEREL NONE)
# Set by the packaging tools while they build: flatpak-builder, snapcraft,
# dpkg-buildpackage (dpkg-architecture) and rpmbuild.
set(GROUNDHOG_PREVIEW_PACKAGING_ENV FLATPAK_ID SNAPCRAFT_PROJECT_NAME DEB_HOST_ARCH
    RPM_BUILD_ROOT)

function(groundhog_preview_problem out)
  cmake_parse_arguments(ARG "CPACK" "BUILD_TYPE;INSTALL_PREFIX" "CONFIGURATION_TYPES" ${ARGN})
  set(problem "")
  string(TOUPPER "${ARG_BUILD_TYPE}" type)
  if (type IN_LIST GROUNDHOG_PREVIEW_RELEASE_TYPES)
    set(problem "CMAKE_BUILD_TYPE is ${ARG_BUILD_TYPE}, a release or packaging build type")
  endif()
  foreach (config IN LISTS ARG_CONFIGURATION_TYPES)
    string(TOUPPER "${config}" upper)
    if (NOT problem AND upper IN_LIST GROUNDHOG_PREVIEW_RELEASE_TYPES)
      set(problem "CMAKE_CONFIGURATION_TYPES includes ${config}, a release build type")
    endif()
  endforeach()
  if (NOT problem AND ARG_CPACK)
    set(problem "CPack is configured")
  endif()
  if (NOT problem AND ARG_INSTALL_PREFIX MATCHES "^/(usr|app)/?$|^/snap(/|$)")
    set(problem "CMAKE_INSTALL_PREFIX is ${ARG_INSTALL_PREFIX}, a distribution prefix")
  endif()
  foreach (name IN LISTS GROUNDHOG_PREVIEW_PACKAGING_ENV)
    if (NOT problem AND DEFINED ENV{${name}})
      set(problem "the environment is a package build (${name} is set)")
    endif()
  endforeach()
  set(${out} "${problem}" PARENT_SCOPE)
endfunction()
