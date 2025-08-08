project(libnostr)

# Core Nostr library. Provides event types, relays, subscriptions, filters, and utils.
find_library(NSYNC_LIB nsync REQUIRED)
find_path(NSYNC_INCLUDE_DIR nsync.h REQUIRED)

# Include the header files
include_directories(include)
include_directories(${NSYNC_INCLUDE_DIR})

# Find the libwebsockets package
find_package(PkgConfig REQUIRED)
pkg_check_modules(LWS REQUIRED libwebsockets)
# Optionally enable websockets support by uncommenting the lines below and
# ensuring libwebsockets is available on the system.
# pkg_check_modules(LWS REQUIRED libwebsockets)
# include_directories(${LWS_INCLUDE_DIRS})

set(LIBNOSTR_SOURCES
    src/timestamp.c
    src/tag.c
    src/event.c
	src/envelope.c
    src/filter.c
	src/connection.c
    src/relay.c
    src/subscription.c
	src/keys.c
	src/json.c
	src/pointer.c
	src/relay_store.c
	src/simplepool.c
    src/utils.c
)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)


# Create the library as a position-independent static lib by default.
add_library(libnostr ${LIBNOSTR_SOURCES})

# Warnings and optional -Werror
option(GO_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
target_compile_options(libnostr PRIVATE -Wall -Wextra -Wpedantic $<$<BOOL:${GO_WARNINGS_AS_ERRORS}>:-Werror>)

# Sanitizer options (Debug builds recommended)
option(GO_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(GO_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(GO_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  if (GO_ENABLE_TSAN)
    target_compile_options(libnostr PRIVATE -fsanitize=thread)
    target_link_options(libnostr PRIVATE -fsanitize=thread)
  else()
    if (GO_ENABLE_ASAN)
      target_compile_options(libnostr PRIVATE -fsanitize=address)
      target_link_options(libnostr PRIVATE -fsanitize=address)
    endif()
    if (GO_ENABLE_UBSAN)
      target_compile_options(libnostr PRIVATE -fsanitize=undefined)
      target_link_options(libnostr PRIVATE -fsanitize=undefined)
    endif()
  endif()
endif()

# Link against libgo (concurrency primitives) and libwebsockets if enabled.
target_include_directories(libnostr PUBLIC ${LWS_INCLUDE_DIRS})
target_link_directories(libnostr PUBLIC ${LWS_LIBRARY_DIRS})
find_package(OpenSSL REQUIRED)
find_library(SECP256K1_LIB NAMES secp256k1 REQUIRED)
target_link_libraries(libnostr PUBLIC ${LWS_LIBRARIES} libgo OpenSSL::Crypto ${SECP256K1_LIB})

target_include_directories(libnostr PUBLIC include)

# Optionally, you can specify installation rules
# install(TARGETS nostr DESTINATION lib)
# install(DIRECTORY include/ DESTINATION include)
