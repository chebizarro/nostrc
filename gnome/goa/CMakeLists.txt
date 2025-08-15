add_library(goa-gnostr MODULE
  provider-gnostr.c
  provider-gnostr.h
)

# Dependencies
# Expect PkgConfig::GOA, PkgConfig::GLIB, PkgConfig::GTK4, PkgConfig::LIBSECRET provided by top-level

target_link_libraries(goa-gnostr PRIVATE
  PkgConfig::GOA_BACKEND
  PkgConfig::GLIB
  PkgConfig::GTK4
  PkgConfig::LIBSECRET
)

target_compile_features(goa-gnostr PRIVATE c_std_11)

target_include_directories(goa-gnostr PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(goa-gnostr PRIVATE GOA_API_IS_SUBJECT_TO_CHANGE=1)

set_target_properties(goa-gnostr PROPERTIES
  OUTPUT_NAME goa-gnostr
)

install(TARGETS goa-gnostr
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/goa-1.0/providers)

# Provider descriptor
install(FILES gnostr.provider
        DESTINATION ${CMAKE_INSTALL_DATADIR}/goa-1.0/providers)

# GSettings schema (account prefs)
install(FILES org.gnostr.goa.gschema.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas)

# Small demo
add_executable(goa-client-demo goa-client-demo.c)

target_link_libraries(goa-client-demo PRIVATE PkgConfig::GOA PkgConfig::GLIB)
target_compile_definitions(goa-client-demo PRIVATE GOA_API_IS_SUBJECT_TO_CHANGE=1)

install(TARGETS goa-client-demo RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
