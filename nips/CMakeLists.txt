project(nips)

# Ensure crypto deps are defined first so targets exist for downstream links
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/nip04/CMakeLists.txt)
  add_subdirectory(nip04)
endif()
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/nip44/CMakeLists.txt)
  add_subdirectory(nip44)
endif()
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/nip19/CMakeLists.txt)
  add_subdirectory(nip19)
endif()

# Loop through all the other NIP directories, excluding ones already added
file(GLOB NIP_DIRS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *)
foreach(NIP_DIR ${NIP_DIRS})
  if (NIP_DIR STREQUAL "nip04" OR NIP_DIR STREQUAL "nip44" OR NIP_DIR STREQUAL "nip19")
    continue()
  endif()
  if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${NIP_DIR}/CMakeLists.txt)
    add_subdirectory(${NIP_DIR})
  endif()
endforeach()
