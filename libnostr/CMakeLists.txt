project(libnostr)

# Include the header files
include_directories(include)

set(LIBNOSTR_SOURCES
    src/timestamp.c
    src/tag.c
    src/event.c
    src/filter.c
    src/relay.c
    src/subscription.c
	src/keys.c
	src/json.c
	src/pointer.c
	src/relay_store.c
	src/simplepool.c
)

# Create the library
add_library(libnostr ${LIBNOSTR_SOURCES})

target_link_libraries(libnostr libgo)

target_include_directories(libnostr PUBLIC include)

# Optionally, you can specify installation rules
# install(TARGETS nostr DESTINATION lib)
# install(DIRECTORY include/ DESTINATION include)
