#!/bin/bash

# Define the path to the nips directory and the output CMake file
NIPS_DIR="nips"
OPTIONS_FILE="NipOptions.cmake"

# Define the content of the CMakeLists.txt template for each NIP
CMAKE_TEMPLATE='
project(%NIP_NAME%)

# Include the header files
include_directories(include ${CMAKE_SOURCE_DIR}/libnostr/include)

# Add the source files
file(GLOB SOURCES "src/*.c")

# Create the library
add_library(%NIP_NAME% ${SOURCES})
target_link_libraries(%NIP_NAME% nostr)

# Add tests if available
file(GLOB TEST_SOURCES "tests/*.c")
if(TEST_SOURCES)
    add_executable(test_%NIP_NAME% ${TEST_SOURCES})
    target_link_libraries(test_%NIP_NAME% %NIP_NAME% nostr)
    add_test(NAME test_%NIP_NAME% COMMAND test_%NIP_NAME%)
endif()
'

# Initialize the options file
echo "# This file is generated by generate_cmake.sh" > "$OPTIONS_FILE"
echo "# NIP options" >> "$OPTIONS_FILE"

# Iterate through each directory in the nips directory
for dir in "$NIPS_DIR"/*; do
    if [ -d "$dir" ]; then
        # Get the name of the NIP directory and convert to uppercase
        NIP_NAME=$(basename "$dir")
        UPPER_NIP_NAME=$(echo "$NIP_NAME" | tr '[:lower:]' '[:upper:]')

        # Add ENABLE_NIP-X option to the options file
        OPTION="option(ENABLE_${UPPER_NIP_NAME} \"Enable ${NIP_NAME}\" OFF)"
        echo "$OPTION" >> "$OPTIONS_FILE"

        # Add conditional to include NIP CMakeLists.txt if ENABLE_NIP-X is ON
        INCLUDE_CONDITION="if(ENABLE_${UPPER_NIP_NAME})\n    add_subdirectory(${NIPS_DIR}/${NIP_NAME})\nendif()\n"
        echo -e "\n$INCLUDE_CONDITION" >> "$OPTIONS_FILE"

        # Check if the directory contains a CMakeLists.txt file
        if [ ! -f "$dir/CMakeLists.txt" ]; then
            # Create the CMakeLists.txt file with the template content
            echo "Creating CMakeLists.txt in $dir"
            echo "$CMAKE_TEMPLATE" | sed "s/%NIP_NAME%/$NIP_NAME/g" > "$dir/CMakeLists.txt"
        else
            echo "CMakeLists.txt already exists in $dir"
        fi
    fi
done

echo "Done."

# Instructions to include the generated file in the main CMakeLists.txt
echo -e "\nTo include the generated NipOptions.cmake, add the following line to your main CMakeLists.txt:\ninclude(\${CMAKE_SOURCE_DIR}/NipOptions.cmake)"
