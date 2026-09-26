# Asserts a configured D-Bus activation file is installable as-is:
# no unconfigured @VAR@ literal survived, and Exec= is an absolute path.
#
#   cmake -DSERVICE_FILE=<path> -P CheckDbusServiceFile.cmake
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
message(STATUS "${SERVICE_FILE}: configured, absolute Exec=")
