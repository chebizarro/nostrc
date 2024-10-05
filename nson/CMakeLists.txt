project(nson)

# Include the header files
include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/libnostr/include)

# Add the source files
set(SOURCES nson.c)

# Create the library
add_library(nson ${SOURCES})
target_link_libraries(nson libnostr libgo)

# Add example executable
add_executable(example example.c)
target_link_libraries(example nson libnostr)

# Add tests
#add_executable(test_nson test_nson.c)
#target_link_libraries(test_nson nson nostr)
#add_test(NAME test_nson COMMAND test_nson)
