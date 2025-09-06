cmake_minimum_required(VERSION 3.16)
project(nip86 C)

add_library(nip86 STATIC
  src/nip86.c
)

target_include_directories(nip86 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(nip86 PRIVATE ${CMAKE_SOURCE_DIR}/third_party/nostrdb/deps/flatcc/include)

find_package(OpenSSL REQUIRED)

target_link_libraries(nip86 PRIVATE nostr nostr_json OpenSSL::Crypto)
