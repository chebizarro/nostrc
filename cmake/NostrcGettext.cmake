# Gettext catalogs for nostrc components (nostrc-gofet.10).
#
#   nostrc_gettext(DOMAIN <domain> PODIR <dir> SOURCE_ROOT <dir>
#                  [DESKTOP <template.in> <output>] [METAINFO <template> <output>]
#                  [INSTALL] [REQUIRED])
#
# Without GNU gettext (xgettext, msgfmt) a REQUIRED component fails to
# configure; any other logs a warning and builds no catalogs (no <domain>-i18n
# target), so library-only builds on hosts without gettext still work.
#
# PODIR holds POTFILES (paths relative to SOURCE_ROOT, one per line; C and
# Blueprint sources, plus .desktop.in/.metainfo.xml templates) and LINGUAS.
# A LINGUAS entry ending in "@pseudo" is generated from the extracted POT by
# nostrc-pseudo-po.py; every other entry is PODIR/<lang>.po. Outputs:
#   <build>/po/<domain>.pot               extracted template (always rebuilt)
#   <build>/locale/<lang>/LC_MESSAGES/<domain>.mo
#   DESKTOP/METAINFO outputs merged with msgfmt --desktop / --xml
# Targets: <domain>-i18n (ALL) and <domain>-pot, which copies the template to
# PODIR/<domain>.pot for translators. Sets <domain>_LOCALE_BUILD_DIR in the
# caller's scope so tests can bind the uninstalled catalogs.
include_guard(GLOBAL)
include(GNUInstallDirs)

find_program(NOSTRC_XGETTEXT xgettext)
find_program(NOSTRC_MSGFMT msgfmt)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(NOSTRC_PSEUDO_PO ${CMAKE_CURRENT_LIST_DIR}/nostrc-pseudo-po.py CACHE INTERNAL "pseudo-locale generator")

function(nostrc_gettext)
  cmake_parse_arguments(G "INSTALL;REQUIRED" "DOMAIN;PODIR;SOURCE_ROOT" "DESKTOP;METAINFO" ${ARGN})
  if (NOT NOSTRC_XGETTEXT OR NOT NOSTRC_MSGFMT)
    if (G_REQUIRED)
      message(FATAL_ERROR "${G_DOMAIN} localization needs GNU gettext (xgettext, msgfmt); install gettext")
    endif()
    message(WARNING "GNU gettext not found: ${G_DOMAIN} is built without translations")
    return()
  endif()
  set(build_po ${CMAKE_CURRENT_BINARY_DIR}/po)
  set(build_locale ${CMAKE_CURRENT_BINARY_DIR}/locale)
  file(MAKE_DIRECTORY ${build_po})

  file(STRINGS ${G_PODIR}/POTFILES potfiles REGEX "^[^#]")
  file(STRINGS ${G_PODIR}/LINGUAS linguas_raw REGEX "^[^#]")
  set(linguas)
  foreach (line IN LISTS linguas_raw)
    string(REGEX REPLACE "[ \t]+" ";" words "${line}")
    foreach (w IN LISTS words)
      if (w)
        list(APPEND linguas ${w})
      endif()
    endforeach()
  endforeach()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               ${G_PODIR}/POTFILES ${G_PODIR}/LINGUAS)

  set(code_sources)
  set(desktop_sources)
  set(xml_sources)
  foreach (f IN LISTS potfiles)
    string(STRIP "${f}" f)
    if (NOT f)
      continue()
    endif()
    if (NOT EXISTS ${G_SOURCE_ROOT}/${f})
      message(FATAL_ERROR "${G_PODIR}/POTFILES lists missing file ${f}")
    endif()
    if (f MATCHES "\\.desktop\\.in$")
      list(APPEND desktop_sources ${f})
    elseif (f MATCHES "\\.xml(\\.in)?$")
      list(APPEND xml_sources ${f})
    else()
      list(APPEND code_sources ${f})
    endif()
  endforeach()

  set(pot ${build_po}/${G_DOMAIN}.pot)
  set(xg ${NOSTRC_XGETTEXT} --package-name=${G_DOMAIN} --from-code=UTF-8
         --add-comments=TRANSLATORS --add-comments=Translators --sort-by-file --directory=${G_SOURCE_ROOT}
         --output=${pot})
  set(extract
    # Blueprint _("…")/C_("ctx", "…") calls parse as C.
    COMMAND ${xg} --language=C --keyword=_ --keyword=N_ --keyword=C_:1c,2
            --keyword=NC_:1c,2 --keyword=ngettext:1,2 --keyword=g_dngettext:2,3
            --keyword=g_dpgettext2:2c,3 --flag=g_strdup_printf:1:c-format
            --flag=g_string_append_printf:2:c-format ${code_sources})
  foreach (f IN LISTS desktop_sources xml_sources)
    list(APPEND extract COMMAND ${xg} --join-existing ${f})
  endforeach()
  set(abs_sources)
  foreach (f IN LISTS potfiles)
    string(STRIP "${f}" f)
    if (f)
      list(APPEND abs_sources ${G_SOURCE_ROOT}/${f})
    endif()
  endforeach()
  add_custom_command(OUTPUT ${pot} ${extract}
    DEPENDS ${abs_sources} ${G_PODIR}/POTFILES
    COMMENT "Extracting ${G_DOMAIN}.pot" VERBATIM)

  set(outputs ${pot})
  set(po_files)
  string(REPLACE ";" " " linguas_line "${linguas}")
  file(WRITE ${build_po}/LINGUAS.tmp "${linguas_line}\n")
  configure_file(${build_po}/LINGUAS.tmp ${build_po}/LINGUAS COPYONLY)
  foreach (lang IN LISTS linguas)
    set(po ${build_po}/${lang}.po)
    if (lang MATCHES "@pseudo$")
      add_custom_command(OUTPUT ${po}
        COMMAND ${Python3_EXECUTABLE} ${NOSTRC_PSEUDO_PO} ${pot} ${po} ${lang}
        DEPENDS ${pot} ${NOSTRC_PSEUDO_PO} VERBATIM)
    else()
      add_custom_command(OUTPUT ${po}
        COMMAND ${CMAKE_COMMAND} -E copy ${G_PODIR}/${lang}.po ${po}
        DEPENDS ${G_PODIR}/${lang}.po VERBATIM)
    endif()
    set(mo ${build_locale}/${lang}/LC_MESSAGES/${G_DOMAIN}.mo)
    add_custom_command(OUTPUT ${mo}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${build_locale}/${lang}/LC_MESSAGES
      COMMAND ${NOSTRC_MSGFMT} --check --output-file=${mo} ${po}
      DEPENDS ${po} VERBATIM)
    list(APPEND outputs ${po} ${mo})
    list(APPEND po_files ${po})
    if (G_INSTALL)
      install(FILES ${mo} DESTINATION ${CMAKE_INSTALL_LOCALEDIR}/${lang}/LC_MESSAGES)
    endif()
  endforeach()

  if (G_DESKTOP)
    list(GET G_DESKTOP 0 tmpl)
    list(GET G_DESKTOP 1 out)
    add_custom_command(OUTPUT ${out}
      COMMAND ${NOSTRC_MSGFMT} --desktop --template=${tmpl} -d ${build_po} -o ${out}
      DEPENDS ${tmpl} ${po_files} ${build_po}/LINGUAS VERBATIM)
    list(APPEND outputs ${out})
  endif()
  if (G_METAINFO)
    list(GET G_METAINFO 0 tmpl)
    list(GET G_METAINFO 1 out)
    add_custom_command(OUTPUT ${out}
      COMMAND ${NOSTRC_MSGFMT} --xml --template=${tmpl} -d ${build_po} -o ${out}
      DEPENDS ${tmpl} ${po_files} ${build_po}/LINGUAS VERBATIM)
    list(APPEND outputs ${out})
  endif()

  add_custom_target(${G_DOMAIN}-i18n ALL DEPENDS ${outputs})
  add_custom_target(${G_DOMAIN}-pot
    COMMAND ${CMAKE_COMMAND} -E copy ${pot} ${G_PODIR}/${G_DOMAIN}.pot
    DEPENDS ${pot} VERBATIM)
  set(${G_DOMAIN}_LOCALE_BUILD_DIR ${build_locale} PARENT_SCOPE)
  set_property(GLOBAL PROPERTY NOSTRC_LOCALE_BUILD_DIR_${G_DOMAIN} ${build_locale})
endfunction()
