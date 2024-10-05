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
)

# Create the library
add_library(libgo ${LIBGO_SOURCES})

target_include_directories(libgo PUBLIC include)


# Optionally, you can specify installation rules
# install(TARGETS nostr DESTINATION lib)
# install(DIRECTORY include/ DESTINATION include)
