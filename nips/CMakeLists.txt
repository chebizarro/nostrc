project(nips)

# Loop through all the NIP directories
file(GLOB NIP_DIRS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *)

foreach(NIP ${NIP_DIRS})
    if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${NIP})
        add_subdirectory(${NIP})
    endif()
endforeach()
