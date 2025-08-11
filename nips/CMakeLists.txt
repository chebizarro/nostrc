project(nips)

# Build with a priority order for NIPs that provide crypto primitives
set(PRIORITY_NIPS nip04 nip44 nip19)
set(ADDED_NIPS "")

foreach(PN ${PRIORITY_NIPS})
  if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${PN}/CMakeLists.txt)
    add_subdirectory(${PN})
    list(APPEND ADDED_NIPS ${PN})
  endif()
endforeach()

# Add all remaining NIPs once
file(GLOB NIP_DIRS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *)
foreach(NIP_DIR ${NIP_DIRS})
  if (NOT IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${NIP_DIR})
    continue()
  endif()
  if (";${ADDED_NIPS};" MATCHES ";${NIP_DIR};")
    continue()
  endif()
  if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${NIP_DIR}/CMakeLists.txt)
    add_subdirectory(${NIP_DIR})
    list(APPEND ADDED_NIPS ${NIP_DIR})
  endif()
endforeach()
