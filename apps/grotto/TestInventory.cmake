include_guard(DIRECTORY)
if(NOT COMMAND nostrc_describe_test)
  return()
endif()

if(TEST grotto_dbus_service_configured)
  nostrc_describe_test(grotto_dbus_service_configured OWNER grotto.daemon
    INPUTS "${CMAKE_SOURCE_DIR}/cmake/CheckDbusServiceFile.cmake"
      "${CMAKE_CURRENT_SOURCE_DIR}/data/org.nostr.Signer.service.in"
      "${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Signer.service")
endif()
if(TEST grotto_app_dbus_service_configured)
  nostrc_describe_test(grotto_app_dbus_service_configured OWNER grotto.gui
    INPUTS "${CMAKE_SOURCE_DIR}/cmake/CheckDbusServiceFile.cmake"
      "${CMAKE_CURRENT_SOURCE_DIR}/data/org.nostr.Grotto.service.in"
      "${CMAKE_CURRENT_SOURCE_DIR}/data/org.nostr.Grotto.desktop"
      "${CMAKE_CURRENT_BINARY_DIR}/org.nostr.Grotto.service")
endif()
