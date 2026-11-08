# Shared packaging contract. VERSION is the component's full release version;
# METAINFO and DESKTOP name the files shipped by its packaging frontend.
function(check_desktop_metadata METAINFO DESKTOP VERSION)
  find_program(APPSTREAMCLI appstreamcli)
  find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
  if(NOT APPSTREAMCLI)
    message(FATAL_ERROR "Desktop metadata validation requires appstreamcli (install appstream)")
  endif()
  if(NOT DESKTOP_FILE_VALIDATE)
    message(FATAL_ERROR "Desktop metadata validation requires desktop-file-validate (install desktop-file-utils)")
  endif()
  execute_process(COMMAND "${APPSTREAMCLI}" validate --strict --no-net "${METAINFO}"
    RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Strict AppStream validation failed for ${METAINFO}: ${result}")
  endif()
  execute_process(COMMAND "${DESKTOP_FILE_VALIDATE}" "${DESKTOP}"
    RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Desktop entry validation failed for ${DESKTOP}: ${result}")
  endif()
  file(READ "${METAINFO}" metadata)
  # AppStream release lists are newest first. Matching any historical release
  # would allow an old entry to conceal stale packaging metadata.
  string(REGEX MATCH "<release[ \t\r\n]+[^>]*version=\"([^\"]+)\"" newest "${metadata}")
  if(NOT newest OR NOT CMAKE_MATCH_1 STREQUAL VERSION)
    message(FATAL_ERROR "${METAINFO}: newest release must equal full version ${VERSION} (found '${CMAKE_MATCH_1}')")
  endif()
endfunction()
