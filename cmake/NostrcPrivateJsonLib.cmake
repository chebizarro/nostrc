# The JSON backend (libnostr-json) is a shared library even when every other
# in-tree library is linked statically: it resolves libnostr's symbols from
# the executable that loads it. An app-only install
# (cmake --install <build>/<app directory>) would leave it behind, and the
# app would start only where some other package happened to provide it.
#
# nostrc_install_private_json(<name> <target>...) installs a private copy in
# <libdir>/<name>/ and points the targets' install RUNPATH at it, relative to
# the binary so the result also works relocated (AppImage, Flatpak's /app).
function(nostrc_install_private_json name)
  if (APPLE OR WIN32 OR NOT TARGET nostr_json)
    return()
  endif()
  get_target_property(_type nostr_json TYPE)
  if (NOT _type STREQUAL "SHARED_LIBRARY")
    return()
  endif()
  install(FILES $<TARGET_FILE:nostr_json>
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/${name}
          RENAME $<TARGET_SONAME_FILE_NAME:nostr_json>)
  file(RELATIVE_PATH _rel "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}/${name}")
  foreach(_target IN LISTS ARGN)
    set_property(TARGET ${_target} APPEND PROPERTY INSTALL_RPATH "\$ORIGIN/${_rel}")
  endforeach()
endfunction()
