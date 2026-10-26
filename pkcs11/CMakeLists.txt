add_library(gnostr-pkcs11 MODULE gnostr-p11.c gnostr-p11.h)

target_link_libraries(gnostr-pkcs11 PRIVATE PkgConfig::P11KIT PkgConfig::GLIB)

set_target_properties(gnostr-pkcs11 PROPERTIES OUTPUT_NAME gnostr-pkcs11)
# PKCS#11 v2.40 modules are entered only through C_GetFunctionList.
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/NostrcVisibility.cmake")
nostrc_module_visibility(gnostr-pkcs11 SYMBOLS C_GetFunctionList)

install(TARGETS gnostr-pkcs11 LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES gnostr.module DESTINATION ${CMAKE_INSTALL_DATADIR}/p11-kit/modules)
