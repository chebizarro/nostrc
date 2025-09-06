project(nip45)

include_directories(
  include
  ${CMAKE_SOURCE_DIR}/libnostr/include
)

add_library(nip45 src/nip45.c)

target_link_libraries(nip45 PUBLIC nostr)
