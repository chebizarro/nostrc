# SPDX-License-Identifier: MIT
# GTK-Doc scaffold for the Signet public C API.
#
# To generate documentation from the standalone Signet Meson build, enable this
# subdir from signet/meson.build after gtk-doc is available, then run:
#
#   meson setup builddir -Dsignet_enable_tests=false
#   meson compile -C builddir signet-doc
#
# The public headers are documented with GTK-Doc comments and are grouped by
# signet-docs.xml.  gi-docgen can also consume the same annotations after a GIR
# target is added for Signet.

gtkdoc = import('gnome')

gtkdoc.gtkdoc('signet',
  main_xml: 'signet-docs.xml',
  src_dir: [
    include_directories('../include/signet'),
  ],
  scan_args: [
    '--rebuild-types',
    '--rebuild-sections',
    '--ignore-headers=store_passkeys_schema.h dbus_common.h',
  ],
  content_files: [],
  install: false,
)
