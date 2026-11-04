# SPDX-License-Identifier: MIT
project('grotto-panels', 'c', version: '0.1.0', license: 'MIT',
  meson_version: '>=0.61', default_options: ['c_std=c11', 'warning_level=2'])
gtk_dep = dependency('gtk4', version: '>=4.14')
adw_dep = dependency('libadwaita-1', version: '>=1.5')
pins = ['-DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_4_14', '-DGDK_VERSION_MAX_ALLOWED=GDK_VERSION_4_14',
  '-DGTK_VERSION_MIN_REQUIRED=GDK_VERSION_4_14', '-DGTK_VERSION_MAX_ALLOWED=GDK_VERSION_4_14',
  '-DADW_VERSION_MIN_REQUIRED=ADW_VERSION_1_5', '-DADW_VERSION_MAX_ALLOWED=ADW_VERSION_1_5']
add_project_arguments(pins, '-Werror=deprecated-declarations', language: 'c')
version_data = configuration_data()
parts = meson.project_version().split('.')
version_data.set('GROTTO_PANELS_VERSION_MAJOR', parts[0])
version_data.set('GROTTO_PANELS_VERSION_MINOR', parts[1])
version_data.set('GROTTO_PANELS_VERSION_MICRO', parts[2])
version_data.set_quoted('GROTTO_PANELS_VERSION', meson.project_version())
configure_file(output: 'grotto-panels-version.h', configuration: version_data,
  install_dir: get_option('includedir') / 'grotto-panels-0')
inc = include_directories('include/grotto-panels-0', '.')
panels = shared_library('grotto-panels-0', 'src/grotto-panel.c', 'src/grotto-panels-service.c',
  include_directories: inc, dependencies: [gtk_dep, adw_dep],
  c_args: ['-DGROTTO_PANELS_COMPILATION'], gnu_symbol_visibility: 'hidden',
  version: meson.project_version(), soversion: '0', install: true)
install_headers('include/grotto-panels-0/grotto-panels.h', subdir: 'grotto-panels-0')
import('pkgconfig').generate(panels, filebase: 'grotto-panels-0', name: 'Grotto Panels',
  description: 'Injected public-metadata management panel scaffold',
  subdirs: 'grotto-panels-0', requires: ['gtk4 >= 4.14', 'libadwaita-1 >= 1.5'])
panels_dep = declare_dependency(link_with: panels, include_directories: inc, dependencies: [gtk_dep, adw_dep])
meson.override_dependency('grotto-panels-0', panels_dep)
if get_option('tests')
  python = import('python').find_installation('python3')
  smoke = executable('test-grotto-panels-smoke', 'tests/test-smoke-host.c', 'tests/fake-service.c', dependencies: panels_dep)
  if host_machine.system() == 'linux'
    runner = find_program('dbus-run-session')
    xvfb = find_program('xvfb-run')
    test('smoke-host', runner, args: ['--', xvfb.full_path(), '-a', smoke.full_path()],
      depends: smoke, suite: 'grotto-panels', timeout: 30, env: ['G_DEBUG=fatal-warnings', 'GTK_A11Y=test',
        'LSAN_OPTIONS=suppressions=' + meson.current_source_dir() / 'tests/lsan.supp' + ':print_suppressions=1'])
  else
    test('smoke-host', smoke, suite: 'grotto-panels', timeout: 30, env: ['G_DEBUG=fatal-warnings', 'GTK_A11Y=test',
        'LSAN_OPTIONS=suppressions=' + meson.current_source_dir() / 'tests/lsan.supp' + ':print_suppressions=1'])
  endif
  test('no-secrets', python, args: [files('tests/test-boundaries.py'), meson.current_source_dir(), panels.full_path()],
    depends: panels, suite: 'grotto-panels')
  control = executable('test-grotto-panels-control1', 'tests/test-control1.c',
    '../../apps/grotto/src/grotto-panels-control1-service.c',
    '../../tests/common/nostrc-test-bus.c',
    include_directories: include_directories('../../apps/grotto/src', '../../tests/common'),
    c_args: ['-D_GNU_SOURCE', '-DGROTTO_CONTROL_XML_PATH="' + meson.current_source_dir() / '../../apps/grotto/data/org.nostr.Grotto.Control1.xml' + '"'],
    dependencies: [panels_dep, dependency('gio-unix-2.0')])
  test('control1-service', control, suite: 'grotto-panels', timeout: 30, env: ['G_DEBUG=fatal-warnings'])
endif
