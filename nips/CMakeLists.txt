project(nips)

# Discover all subdirectories once and add each NIP exactly once
file(GLOB NIP_DIRS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *)
foreach(NIP_DIR ${NIP_DIRS})
  if (NOT IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${NIP_DIR})
    continue()
  endif()
  if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${NIP_DIR}/CMakeLists.txt)
    add_subdirectory(${NIP_DIR})
  endif()
endforeach()
