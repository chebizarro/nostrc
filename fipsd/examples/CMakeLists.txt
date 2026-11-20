# The two programs from upstream docs/how-to/write-a-native-api-client.md and
# use-the-native-datagram-api.md, in C over libfipsnative: an echo server that
# serves one datagram per flow, and a client that sends one datagram and waits
# a bounded time for the answer. Built, not installed.
foreach(_example native-echo native-send)
  add_executable(fipsd_example_${_example} ${_example}.c)
  set_target_properties(fipsd_example_${_example} PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO OUTPUT_NAME ${_example})
  target_link_libraries(fipsd_example_${_example} PRIVATE fipsnative_static)
  apply_warnings(fipsd_example_${_example})
  apply_sanitizers(fipsd_example_${_example})
endforeach()
find_package(Threads REQUIRED)
target_link_libraries(fipsd_example_native-echo PRIVATE Threads::Threads)
