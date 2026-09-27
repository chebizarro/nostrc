# Asserts a configured D-Bus activation file is installable as-is:
# no unconfigured @VAR@ literal survived, and Exec= is an absolute path.
#
#   cmake -DSERVICE_FILE=<path> [-DEXPECT_NAME=<bus name>]
#         [-DDESKTOP_FILE=<path>] -P CheckDbusServiceFile.cmake
#
# EXPECT_NAME: Name= must equal it. DESKTOP_FILE: a DBusActivatable=true
# desktop entry, whose file id (basename without .desktop) must equal Name=,
# which is how GDesktopAppInfo picks the bus name to activate.
if(NOT SERVICE_FILE OR NOT EXISTS "${SERVICE_FILE}")
  message(FATAL_ERROR "service file not found: '${SERVICE_FILE}'")
endif()
file(READ "${SERVICE_FILE}" _content)
if(_content MATCHES "@")
  message(FATAL_ERROR "${SERVICE_FILE} still contains an '@' literal:\n${_content}")
endif()
if(NOT _content MATCHES "\nExec=/[^\n]+")
  message(FATAL_ERROR "${SERVICE_FILE} has no absolute Exec= line:\n${_content}")
endif()
if(DEFINED EXPECT_NAME AND NOT _content MATCHES "\nName=${EXPECT_NAME}\n")
  message(FATAL_ERROR "${SERVICE_FILE} Name= is not ${EXPECT_NAME}:\n${_content}")
endif()
if(DEFINED DESKTOP_FILE)
  get_filename_component(_desktop_id "${DESKTOP_FILE}" NAME_WLE)
  file(READ "${DESKTOP_FILE}" _desktop)
  if(NOT _desktop MATCHES "\nDBusActivatable=true\n")
    message(FATAL_ERROR "${DESKTOP_FILE} is not DBusActivatable=true")
  endif()
  if(NOT _content MATCHES "\nName=${_desktop_id}\n")
    message(FATAL_ERROR "${SERVICE_FILE} Name= does not match desktop id ${_desktop_id}")
  endif()
endif()
message(STATUS "${SERVICE_FILE}: configured, absolute Exec=")
