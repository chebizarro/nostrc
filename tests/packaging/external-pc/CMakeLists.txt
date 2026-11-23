cmake_minimum_required(VERSION 3.22)
project(external_pc_consumer C)
enable_testing()
include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/NostrcPkgConfig.cmake")

set(_libdir "${CMAKE_CURRENT_BINARY_DIR}/lib")
set(_includedir "${CMAKE_CURRENT_BINARY_DIR}/include")
set(_pcdir "${CMAKE_CURRENT_BINARY_DIR}/pkgconfig")
file(MAKE_DIRECTORY "${_libdir}" "${_includedir}" "${_pcdir}")
file(WRITE "${_includedir}/external.h" "#ifndef EXTERNAL_PUBLIC\n#error Missing public Cflags\n#endif\nint external_value(void);\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/external.c" "int external_value(void) { return 42; }\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/internal.c" "int external_value(void);\nint internal_value(void) { return external_value(); }\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/sdk.c" "int internal_value(void);\nint sdk_value(void) { return internal_value(); }\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/consumer.c" "#include <external.h>\nint sdk_value(void);\nint main(void) { return sdk_value() != 42 || external_value() != 42; }\n")
add_library(external-fixture SHARED "${CMAKE_CURRENT_BINARY_DIR}/external.c")
add_library(internal-fixture STATIC "${CMAKE_CURRENT_BINARY_DIR}/internal.c")
add_library(sdk-fixture STATIC "${CMAKE_CURRENT_BINARY_DIR}/sdk.c")
foreach(_target external-fixture internal-fixture sdk-fixture)
  set_target_properties(${_target} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${_libdir}"
    ARCHIVE_OUTPUT_DIRECTORY "${_libdir}")
endforeach()

# A system DSO's unavailable static implementation must not reach our consumer.
# The SDK itself has a private archive dependency, which must remain reachable.
file(WRITE "${_pcdir}/external-fixture.pc" "Name: external-fixture
Description: Shared external library with unavailable static implementation
Version: 1.0
Libs: -L${_libdir} -lexternal-fixture
Libs.private: -lprivate-static-not-installed
Requires.private: external-private-fixture
Cflags: -I${_includedir} -DEXTERNAL_PUBLIC
")
file(WRITE "${_pcdir}/external-private-fixture.pc" "Name: external-private-fixture
Description: Unavailable private system implementation
Version: 1.0
Libs: -lrecursive-static-not-installed
")
set(ENV{PKG_CONFIG_PATH} "${_pcdir}")
set(ENV{PKG_CONFIG_LIBDIR} "${_pcdir}")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_CURRENT_BINARY_DIR}/producer-sysroot")
nostrc_pkg_config_shared(_shared external-fixture)
if("${_shared_LIBS};${_shared_CFLAGS}" MATCHES "producer-sysroot|static-not-installed")
  message(FATAL_ERROR "External shared flags leaked producer/static details: ${_shared_LIBS}; ${_shared_CFLAGS}")
endif()
file(WRITE "${_pcdir}/internal-fixture.pc" "Name: internal-fixture
Description: SDK private archive with ordinary shared system flags
Version: 1.0
Libs: -L${_libdir} -linternal-fixture
Libs.private: ${_shared_LIBS}
Cflags: ${_shared_CFLAGS}
")
file(WRITE "${_pcdir}/sdk-fixture.pc" "Name: sdk-fixture
Description: SDK public archive
Version: 1.0
Requires.private: internal-fixture
Libs: -L${_libdir} -lsdk-fixture
")
# Query as a native downstream consumer, not with the producer's cross sysroot.
unset(ENV{PKG_CONFIG_SYSROOT_DIR})
foreach(_kind libs cflags)
  execute_process(COMMAND "${PKG_CONFIG_EXECUTABLE}" --static --${_kind} sdk-fixture
    OUTPUT_VARIABLE _${_kind} OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
  separate_arguments(_${_kind} UNIX_COMMAND "${_${_kind}}")
endforeach()
foreach(_library sdk-fixture internal-fixture external-fixture)
  if(NOT "-l${_library}" IN_LIST _libs)
    message(FATAL_ERROR "Missing expected consumer library ${_library}: ${_libs}")
  endif()
endforeach()
message(STATUS "Mixed SDK consumer libraries: ${_libs}")
add_executable(consumer "${CMAKE_CURRENT_BINARY_DIR}/consumer.c")
target_compile_options(consumer PRIVATE ${_cflags})
target_link_libraries(consumer PRIVATE ${_libs})
set_target_properties(consumer PROPERTIES BUILD_RPATH "${_libdir}")
add_dependencies(consumer sdk-fixture internal-fixture external-fixture)
add_test(NAME external_pc_mixed_consumer COMMAND consumer)
