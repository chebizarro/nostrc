# gnostr-secret: the one libsecret schema for Nostr key material (nostrc-bml6).
#
# Built whenever a consumer needs it: nips/nip55l adds this directory itself
# when libsecret is available (the signer daemon owns the schema), and the
# top-level GNOSTR_ENABLE_SEAHORSE_HELPERS option adds it for standalone
# installs. Self-contained dependency lookup so either entry point works.
find_package(PkgConfig REQUIRED)
pkg_check_modules(GNOSTR_SECRET_DEPS REQUIRED IMPORTED_TARGET
  glib-2.0 gio-2.0 libsecret-1)

add_library(gnostr-secret STATIC secret_store.c secret_store.h)
target_link_libraries(gnostr-secret PUBLIC PkgConfig::GNOSTR_SECRET_DEPS)
# Consumers include "seahorse/secret_store.h" (the bare name collides with
# apps/gnostr-signer/src/secret_store.h).
target_include_directories(gnostr-secret PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>)
target_compile_features(gnostr-secret PRIVATE c_std_11)
# Linked into libnostr_nip55l_core, which is shared under BUILD_SHARED_LIBS.
set_target_properties(gnostr-secret PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The archive and schema doc are only installed for an explicit helper
# build; as an internal dependency of the signer they are linked in.
if (GNOSTR_ENABLE_SEAHORSE_HELPERS)
  install(TARGETS gnostr-secret ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(FILES org.gnostr.secret.schema.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/gnostr)
endif()
