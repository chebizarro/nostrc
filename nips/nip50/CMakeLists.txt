project(nip50)

include_directories(
  include
  ${CMAKE_SOURCE_DIR}/libnostr/include
)

add_library(nip50 src/nip50.c)

target_link_libraries(nip50 PUBLIC nostr)
