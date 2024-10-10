project(libgo)

# Include the header files
include_directories(include)

set(LIBGO_SOURCES
    src/go.c
    src/channel.c
    src/context.c
	src/wait_group.c
	src/hash_map.c
	src/counter.c
	src/select.c
	src/string_array.c
	src/int_array.c
    src/ticker.c
)

# Create the library
add_library(libgo ${LIBGO_SOURCES})

target_include_directories(libgo PUBLIC include)

target_link_libraries(libgo)

# Optionally, you can specify installation rules
# install(TARGETS nostr DESTINATION lib)
# install(DIRECTORY include/ DESTINATION include)
