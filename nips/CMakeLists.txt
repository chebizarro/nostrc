project(nips)

# Ensure crypto deps are defined first so targets exist for downstream links
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/nip04/CMakeLists.txt)
  add_subdirectory(nip04)
endif()
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/nip44/CMakeLists.txt)
  add_subdirectory(nip44)
endif()

# Loop through all the other NIP directories
file(GLOB NIP_DIRS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *)

foreach(NIP ${NIP_DIRS})
    if(NIP STREQUAL "nip04" OR NIP STREQUAL "nip44")
        continue()
    endif()
    if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${NIP})
        add_subdirectory(${NIP})
    endif()
endforeach()
