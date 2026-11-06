# SPDX-License-Identifier: MIT
project('grotto-panels', 'c', version: '0.1.0', license: 'MIT',
  meson_version: '>=0.61', default_options: ['c_std=c11', 'warning_level=2'])
gtk_dep = dependency('gtk4', version: '>=4.14')
adw_dep = dependency('libadwaita-1', version: '>=1.5')
intl_dep = dependency('intl', required: false)
subdir('po')
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
resources = import('gnome').compile_resources('grotto-panels-resources', 'data/grotto-panels.gresource.xml', source_dir: 'data', export: false)
panels = shared_library('grotto-panels-0', resources, 'src/grotto-app-info.c', 'src/grotto-apps-panel.c', 'src/grotto-app-detail.c', 'src/grotto-activity-panel.c', 'src/grotto-remote-panel.c', 'src/grotto-identity-panel.c', 'src/grotto-profile-view.c', 'src/grotto-panel.c', 'src/grotto-panels-service.c', 'src/grotto-panels-validation.c',
  include_directories: inc, dependencies: [gtk_dep, adw_dep, intl_dep],
  c_args: ['-DGROTTO_PANELS_COMPILATION', '-DGETTEXT_PACKAGE="grotto-panels"',
    '-DGROTTO_PANELS_LOCALEDIR="@0@"'.format(get_option('prefix') / get_option('localedir'))], gnu_symbol_visibility: 'hidden',
  version: meson.project_version(), soversion: '0', install: true)
install_headers('include/grotto-panels-0/grotto-panels.h', subdir: 'grotto-panels-0')
import('pkgconfig').generate(panels, filebase: 'grotto-panels-0', name: 'Grotto Panels',
  description: 'Injected public-metadata management panel scaffold',
  subdirs: 'grotto-panels-0', requires: ['gtk4 >= 4.14', 'libadwaita-1 >= 1.5'])
panels_dep = declare_dependency(link_with: panels, include_directories: inc, dependencies: [gtk_dep, adw_dep])
meson.override_dependency('grotto-panels-0', panels_dep)
if get_option('tests')
  python = import('python').find_installation('python3')
  clang = find_program('clang')
  admission = executable('test-grotto-panels-admission', 'tests/test-admission.c', 'tests/fake-service.c', dependencies: panels_dep)
  smoke_deps = [panels_dep]
  if host_machine.system() == 'linux'
    smoke_deps += meson.get_compiler('c').find_library('dl')
  endif
  smoke = executable('test-grotto-panels-smoke', 'tests/test-smoke-host.c', 'tests/test-management-panels.c', 'tests/test-profile-panel.c', 'tests/fake-service.c',
    include_directories: include_directories('../../tests/common'), dependencies: smoke_deps)
  if host_machine.system() == 'linux'
    runner = find_program('dbus-run-session')
    xvfb = find_program('xvfb-run')
    test('smoke-host', runner, args: ['--', xvfb.full_path(), '-a', smoke.full_path()],
      depends: smoke, suite: 'grotto-panels', timeout: 30, env: ['XDG_DATA_DIRS=' + meson.current_source_dir() / 'tests/fixtures' + ':/usr/local/share:/usr/share', 'G_DEBUG=fatal-warnings', 'GTK_A11Y=test',
        'LSAN_OPTIONS=suppressions=' + meson.current_source_dir() / 'tests/lsan.supp' + ':print_suppressions=1'])
  else
    test('smoke-host', smoke, suite: 'grotto-panels', timeout: 30, env: ['XDG_DATA_DIRS=' + meson.current_source_dir() / 'tests/fixtures' + ':/usr/local/share:/usr/share', 'G_DEBUG=fatal-warnings', 'GTK_A11Y=test',
        'LSAN_OPTIONS=suppressions=' + meson.current_source_dir() / 'tests/lsan.supp' + ':print_suppressions=1'])
  endif
  foreach focused : [['profile-panel', '/grotto-panels/profile'], ['management-panels', '/panels']]
    if host_machine.system() == 'linux'
      test(focused[0], runner, args: ['--', xvfb.full_path(), '-a', smoke.full_path(), '-p', focused[1]],
        depends: smoke, suite: 'grotto-panels', timeout: 30, env: ['XDG_DATA_DIRS=' + meson.current_source_dir() / 'tests/fixtures' + ':/usr/local/share:/usr/share', 'G_DEBUG=fatal-warnings', 'GTK_A11Y=test',
          'LSAN_OPTIONS=suppressions=' + meson.current_source_dir() / 'tests/lsan.supp' + ':print_suppressions=1'])
    else
      test(focused[0], smoke, args: ['-p', focused[1]], suite: 'grotto-panels', timeout: 30,
        env: ['XDG_DATA_DIRS=' + meson.current_source_dir() / 'tests/fixtures' + ':/usr/local/share:/usr/share', 'G_DEBUG=fatal-warnings', 'GTK_A11Y=test',
          'LSAN_OPTIONS=suppressions=' + meson.current_source_dir() / 'tests/lsan.supp' + ':print_suppressions=1'])
    endif
  endforeach
  test('no-secrets', python, args: [files('tests/test-boundaries.py'), meson.current_source_dir(), panels.full_path(),
      admission.full_path(), meson.current_build_dir(), clang.full_path()],
    depends: [panels, admission], suite: 'grotto-panels')
  control = executable('test-grotto-panels-control1', 'tests/test-control1.c',
    '../../apps/grotto/src/grotto-panels-control1-service.c',
    '../../tests/common/nostrc-test-bus.c',
    include_directories: include_directories('../../apps/grotto/src', '../../tests/common'),
    c_args: ['-D_GNU_SOURCE', '-DGROTTO_CONTROL_XML_PATH="' + meson.current_source_dir() / '../../apps/grotto/data/org.nostr.Grotto.Control1.xml' + '"'],
    dependencies: [panels_dep, dependency('gio-unix-2.0')])
  panel_schema = configure_file(input: '../../apps/grotto/data/org.nostr.Grotto.gschema.xml',
    output: 'org.nostr.Grotto.gschema.xml', copy: true)
  panel_test_schemas = custom_target('panel-test-schemas', input: panel_schema,
    output: 'gschemas.compiled', command: [find_program('glib-compile-schemas'), '--strict', meson.current_build_dir()],
    build_by_default: true)
  test('control1-service', control, depends: panel_test_schemas, suite: 'grotto-panels', timeout: 30,
    env: ['G_DEBUG=fatal-warnings', 'GSETTINGS_BACKEND=memory', 'GSETTINGS_SCHEMA_DIR=' + meson.current_build_dir()])
endif
