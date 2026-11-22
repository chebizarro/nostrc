# Initial cache for a reproducible NIP packaging subset. Use only on a fresh tree.
# Keep future unrelated NIPs off rather than inheriting their default dependencies.
file(READ "${CMAKE_CURRENT_LIST_DIR}/../../nips/NipOptions.cmake.in" _nip_options)
string(REGEX MATCHALL "option\\((ENABLE_[A-Z0-9_]+|WITH_NIP[A-Z0-9_]+)" _nip_options "${_nip_options}")
foreach(_option IN LISTS _nip_options)
  string(REPLACE "option(" "" _option "${_option}")
  set(${_option} OFF CACHE BOOL "Not part of the installed NIP subset")
endforeach()
# nip06/nip19 are required by the always-built nip55l core.
foreach(_nip NIP04 NIP06 NIP11 NIP17 NIP19 NIP44 NIP46 NIP51 NIP98)
  set(ENABLE_${_nip} ON CACHE BOOL "Installed NIP subset dependency" FORCE)
endforeach()
foreach(_off BUILD_FIPS WITH_NOSTRDB LIBNOSTR_WITH_NOSTRDB BUILD_APPS BUILD_RELAYD
    SIGNET_ENABLE BUILD_NOSTR_GTK BUILD_NOSTR_GOBJECT BUILD_LIBMARMOT
    BUILD_MARMOT_GOBJECT BUILD_LIBBLOSSOM BUILD_LIBPORTHOME BUILD_LIBVAULT
    BUILD_LIBVAULT_MARMOT BUILD_LIBHANAMI BUILD_TESTING_FRAMEWORK ENABLE_BUD02)
  set(${_off} OFF CACHE BOOL "Not part of the installed NIP subset")
endforeach()
set(NOSTR_USE_SYSTEM_NSYNC ON CACHE STRING "Use the packaged nsync dependency")
# Never access the developer's login Keychain in test builds on macOS.
set(NIP55L_SECRET_BACKEND libsecret CACHE STRING "Private Secret Service for tests")
