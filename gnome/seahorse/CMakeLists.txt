add_library(gnostr-secret STATIC secret_store.c secret_store.h)

target_link_libraries(gnostr-secret PRIVATE PkgConfig::GLIB PkgConfig::LIBSECRET)

target_compile_features(gnostr-secret PRIVATE c_std_11)

install(TARGETS gnostr-secret ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

install(FILES org.gnostr.secret.schema.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/gnostr)
