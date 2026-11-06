# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.22)
project(grotto-panels VERSION 0.1.0 LANGUAGES C)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/VersionHelpers.cmake")
option(ENABLE_COMPONENT_VERSIONING "Enable generated component version headers" ON)
declare_component_version(grotto-panels ${PROJECT_VERSION_MAJOR} ${PROJECT_VERSION_MINOR} ${PROJECT_VERSION_PATCH})
find_package(PkgConfig REQUIRED)
pkg_check_modules(GROTTO_PANELS_DEPS REQUIRED IMPORTED_TARGET gtk4>=4.14 libadwaita-1>=1.5)
set(_pins GDK_VERSION_MIN_REQUIRED=GDK_VERSION_4_14 GDK_VERSION_MAX_ALLOWED=GDK_VERSION_4_14
  GTK_VERSION_MIN_REQUIRED=GDK_VERSION_4_14 GTK_VERSION_MAX_ALLOWED=GDK_VERSION_4_14
  ADW_VERSION_MIN_REQUIRED=ADW_VERSION_1_5 ADW_VERSION_MAX_ALLOWED=ADW_VERSION_1_5)
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/grotto-panels-resources.c"
  COMMAND ${GLIB_COMPILE_RESOURCES} --generate-source --internal --target=${CMAKE_CURRENT_BINARY_DIR}/grotto-panels-resources.c
    --sourcedir=${CMAKE_CURRENT_SOURCE_DIR}/data ${CMAKE_CURRENT_SOURCE_DIR}/data/grotto-panels.gresource.xml
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/data/grotto-panels.gresource.xml ${CMAKE_CURRENT_SOURCE_DIR}/data/identity-panel.ui
    ${CMAKE_CURRENT_SOURCE_DIR}/data/apps-panel.ui ${CMAKE_CURRENT_SOURCE_DIR}/data/activity-panel.ui
    ${CMAKE_CURRENT_SOURCE_DIR}/data/remote-panel.ui ${CMAKE_CURRENT_SOURCE_DIR}/data/icons/scalable/actions/grotto-qr-code-symbolic.svg)
add_library(grotto-panels SHARED src/grotto-panel.c src/grotto-identity-panel.c src/grotto-profile-view.c
  src/grotto-app-info.c src/grotto-apps-panel.c src/grotto-app-detail.c src/grotto-activity-panel.c src/grotto-remote-panel.c
  src/grotto-panels-service.c src/grotto-panels-validation.c "${CMAKE_CURRENT_BINARY_DIR}/grotto-panels-resources.c")
add_library(GrottoPanels::Panels ALIAS grotto-panels)
set_target_properties(grotto-panels PROPERTIES OUTPUT_NAME grotto-panels-0
  VERSION ${PROJECT_VERSION} SOVERSION 0 EXPORT_NAME Panels C_VISIBILITY_PRESET hidden)
target_compile_features(grotto-panels PUBLIC c_std_11)
target_compile_definitions(grotto-panels PRIVATE GROTTO_PANELS_COMPILATION GETTEXT_PACKAGE="grotto-panels"
  GROTTO_PANELS_LOCALEDIR="${CMAKE_INSTALL_FULL_LOCALEDIR}" ${_pins})
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(grotto-panels PRIVATE -Wall -Wextra -Werror=deprecated-declarations)
endif()
target_include_directories(grotto-panels PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/grotto-panels-0>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/grotto-panels-0>)
target_link_libraries(grotto-panels PUBLIC PkgConfig::GROTTO_PANELS_DEPS)
find_package(Intl)
if(Intl_FOUND)
  target_link_libraries(grotto-panels PRIVATE Intl::Intl)
endif()
if(COMMAND apply_sanitizers)
  apply_sanitizers(grotto-panels)
endif()
install(TARGETS grotto-panels EXPORT GrottoPanelsTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/grotto-panels-0/grotto-panels.h
  "${grotto-panels_VERSION_HEADER}" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/grotto-panels-0)
set(_configdir ${CMAKE_INSTALL_LIBDIR}/cmake/GrottoPanels)
configure_package_config_file(cmake/GrottoPanelsConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/GrottoPanelsConfig.cmake" INSTALL_DESTINATION ${_configdir})
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/GrottoPanelsConfigVersion.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(EXPORT GrottoPanelsTargets NAMESPACE GrottoPanels:: DESTINATION ${_configdir})
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/GrottoPanelsConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/GrottoPanelsConfigVersion.cmake" DESTINATION ${_configdir})
configure_file(grotto-panels-0.pc.in grotto-panels-0.pc @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/grotto-panels-0.pc" DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# Keep the shared domain independent of either hosting application's domain.
find_program(GROTTO_PANELS_MSGFMT msgfmt REQUIRED)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/po/LINGUAS" _panel_languages REGEX "^[a-zA-Z_@]+$")
foreach(_lang IN LISTS _panel_languages)
  set(_mo "${CMAKE_CURRENT_BINARY_DIR}/po/${_lang}/LC_MESSAGES/grotto-panels.mo")
  add_custom_command(OUTPUT "${_mo}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/po/${_lang}/LC_MESSAGES"
    COMMAND ${GROTTO_PANELS_MSGFMT} --check --output-file=${_mo} "${CMAKE_CURRENT_SOURCE_DIR}/po/${_lang}.po"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/po/${_lang}.po" VERBATIM)
  list(APPEND _panel_catalogues "${_mo}")
  install(FILES "${_mo}" DESTINATION "${CMAKE_INSTALL_LOCALEDIR}/${_lang}/LC_MESSAGES" COMPONENT grotto-panels-translations)
endforeach()
add_custom_target(grotto-panels-translations ALL DEPENDS ${_panel_catalogues})
add_dependencies(grotto-panels grotto-panels-translations)

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  find_program(GROTTO_PANELS_CLANG clang REQUIRED)
  add_executable(test-grotto-panels-admission tests/test-admission.c tests/fake-service.c)
  target_link_libraries(test-grotto-panels-admission PRIVATE grotto-panels)
  target_compile_definitions(test-grotto-panels-admission PRIVATE ${_pins})
  if(COMMAND apply_sanitizers)
    apply_sanitizers(test-grotto-panels-admission)
  endif()
  add_executable(test-grotto-panels-smoke tests/test-smoke-host.c tests/test-management-panels.c tests/test-profile-panel.c tests/fake-service.c)
  target_link_libraries(test-grotto-panels-smoke PRIVATE grotto-panels ${CMAKE_DL_LIBS})
  target_include_directories(test-grotto-panels-smoke PRIVATE ../../tests/common)
  target_compile_definitions(test-grotto-panels-smoke PRIVATE ${_pins})
  if(COMMAND apply_sanitizers)
    apply_sanitizers(test-grotto-panels-smoke)
  endif()
  # Standalone build on Linux exercises the exact GTK 4.14/adwaita 1.5 floor.
  # Never silently omit the smoke test when its display/bus runner is missing.
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(XVFB_RUN xvfb-run REQUIRED)
    find_program(DBUS_RUN_SESSION dbus-run-session REQUIRED)
    add_test(NAME grotto-panels/smoke-host COMMAND ${DBUS_RUN_SESSION} -- ${XVFB_RUN} -a
      $<TARGET_FILE:test-grotto-panels-smoke>)
  else()
    add_test(NAME grotto-panels/smoke-host COMMAND test-grotto-panels-smoke)
  endif()
  set_tests_properties(grotto-panels/smoke-host PROPERTIES TIMEOUT 30
    ENVIRONMENT "XDG_DATA_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures:/usr/local/share:/usr/share;G_DEBUG=fatal-warnings;GTK_A11Y=test;LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan.supp:print_suppressions=1")
  foreach(focused profile-panel management-panels)
    if(focused STREQUAL "profile-panel")
      set(_panel_path /grotto-panels/profile)
    else()
      set(_panel_path /panels)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      add_test(NAME grotto-panels/${focused} COMMAND ${DBUS_RUN_SESSION} -- ${XVFB_RUN} -a
        $<TARGET_FILE:test-grotto-panels-smoke> -p ${_panel_path})
    else()
      add_test(NAME grotto-panels/${focused} COMMAND test-grotto-panels-smoke -p ${_panel_path})
    endif()
    set_tests_properties(grotto-panels/${focused} PROPERTIES TIMEOUT 30
      ENVIRONMENT "XDG_DATA_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures:/usr/local/share:/usr/share;G_DEBUG=fatal-warnings;GTK_A11Y=test;LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan.supp:print_suppressions=1")
  endforeach()
  add_test(NAME grotto-panels/no-secrets COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-boundaries.py" "${CMAKE_CURRENT_SOURCE_DIR}" $<TARGET_FILE:grotto-panels>
    $<TARGET_FILE:test-grotto-panels-admission> "${CMAKE_CURRENT_BINARY_DIR}/generated" "${GROTTO_PANELS_CLANG}")
  set_tests_properties(grotto-panels/no-secrets PROPERTIES TIMEOUT 60 ENVIRONMENT "G_DEBUG=fatal-warnings")
  include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/NostrcTestBus.cmake")
  find_program(GROTTO_PANELS_COMPILE_SCHEMAS glib-compile-schemas REQUIRED)
  set(_panel_test_schema "${CMAKE_CURRENT_BINARY_DIR}/test-schemas/gschemas.compiled")
  add_custom_command(OUTPUT ${_panel_test_schema}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/test-schemas"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/../../apps/grotto/data/org.nostr.Grotto.gschema.xml"
      "${CMAKE_CURRENT_BINARY_DIR}/test-schemas/"
    COMMAND ${GROTTO_PANELS_COMPILE_SCHEMAS} "${CMAKE_CURRENT_BINARY_DIR}/test-schemas"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../../apps/grotto/data/org.nostr.Grotto.gschema.xml" VERBATIM)
  add_custom_target(grotto-panels-test-schemas DEPENDS ${_panel_test_schema})
  add_executable(test-grotto-panels-control1 tests/test-control1.c
    ../../apps/grotto/src/grotto-panels-control1-service.c)
  target_include_directories(test-grotto-panels-control1 PRIVATE ../../apps/grotto/src ../../tests/common)
  target_link_libraries(test-grotto-panels-control1 PRIVATE grotto-panels nostrc-test-bus)
  target_compile_definitions(test-grotto-panels-control1 PRIVATE
    GROTTO_CONTROL_XML_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../apps/grotto/data/org.nostr.Grotto.Control1.xml")
  target_compile_definitions(test-grotto-panels-control1 PRIVATE ${_pins})
  if(COMMAND apply_sanitizers)
    apply_sanitizers(test-grotto-panels-control1)
  endif()
  add_dependencies(test-grotto-panels-control1 grotto-panels-test-schemas)
  add_test(NAME grotto-panels/control1-service COMMAND test-grotto-panels-control1)
  set_tests_properties(grotto-panels/control1-service PROPERTIES TIMEOUT 30 ENVIRONMENT "G_DEBUG=fatal-warnings;GSETTINGS_SCHEMA_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-schemas;GSETTINGS_BACKEND=memory")
  if(COMMAND nostrc_describe_test)
    nostrc_describe_test(grotto-panels/smoke-host OWNER grotto.panels
      TARGETS test-grotto-panels-smoke INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan.supp")
    foreach(focused profile-panel management-panels)
      nostrc_describe_test(grotto-panels/${focused} OWNER grotto.panels TARGETS test-grotto-panels-smoke
        INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/lsan.supp")
    endforeach()
    nostrc_describe_test(grotto-panels/no-secrets OWNER grotto.panels TARGETS grotto-panels test-grotto-panels-admission
      INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-boundaries.py")
    nostrc_describe_test(grotto-panels/control1-service OWNER grotto.panels
      TARGETS test-grotto-panels-control1
      INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/../../apps/grotto/data/org.nostr.Grotto.Control1.xml")
  endif()
endif()
