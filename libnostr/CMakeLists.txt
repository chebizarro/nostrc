project(libnostr)

# Include the header files
include_directories(include)

# Find the libwebsockets package
find_package(PkgConfig REQUIRED)
pkg_check_modules(LWS REQUIRED libwebsockets)
# Include directories from libwebsockets
include_directories(${LWS_INCLUDE_DIRS})

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

# Create the library
add_library(libnostr ${LIBNOSTR_SOURCES})

target_link_libraries(libnostr libgo ${LWS_LIBRARIES})

target_include_directories(libnostr PUBLIC include)

# Optionally, you can specify installation rules
# install(TARGETS nostr DESTINATION lib)
# install(DIRECTORY include/ DESTINATION include)
