add_library(gnostr-pkcs11 MODULE gnostr-p11.c gnostr-p11.h)

target_link_libraries(gnostr-pkcs11 PRIVATE PkgConfig::P11KIT PkgConfig::GLIB)

set_target_properties(gnostr-pkcs11 PROPERTIES OUTPUT_NAME gnostr-pkcs11)

install(TARGETS gnostr-pkcs11 LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES gnostr.module DESTINATION ${CMAKE_INSTALL_DATADIR}/p11-kit/modules)
