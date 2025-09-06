cmake_minimum_required(VERSION 3.16)
project(relayctl C)

add_executable(nostrc-relayctl src/relayctl_main.c)

find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

target_include_directories(nostrc-relayctl PRIVATE
  ${CMAKE_SOURCE_DIR}/libnostr/include
)

target_link_libraries(nostrc-relayctl PRIVATE
  nostr
  nostr_json
  OpenSSL::Crypto
  CURL::libcurl
)

install(TARGETS nostrc-relayctl RUNTIME DESTINATION bin)
