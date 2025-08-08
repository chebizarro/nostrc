project(libnostr)

# Core Nostr library. Provides event types, relays, subscriptions, filters, and utils.

# Include the header files
include_directories(include)

# Find the libwebsockets package
find_package(PkgConfig REQUIRED)
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

target_compile_options(libnostr PRIVATE $<$<CONFIG:Debug>:-Werror>)

# Link against libgo (concurrency primitives) and libwebsockets if enabled.
target_link_libraries(libnostr libgo ${LWS_LIBRARIES})

target_include_directories(libnostr PUBLIC include)

# Optionally, you can specify installation rules
# install(TARGETS nostr DESTINATION lib)
# install(DIRECTORY include/ DESTINATION include)
