#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compiler schema + closed ELF/Mach-O surface + executable data admission.

This constrains the shipped API, not arbitrary malicious native code or covert
secrets disguised as ordinary public text. It makes no semantic policy claim.
"""
import json
import pathlib
import re
import shlex
import subprocess
import sys
import tempfile

root, library, admission, generated, clang = sys.argv[1:]
root = pathlib.Path(root)
flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', 'gtk4', 'libadwaita-1'], text=True))
include = ['-I' + str(root / 'include/grotto-panels-0'), '-I' + generated]
schemas = {
    '_GrottoPanelsMetadata': {'id': 'const char *', 'label': 'const char *', 'public_key': 'const char *',
        'state': 'const char *', 'reason': 'const char *', 'access_policy': 'const char *',
        'needs_review': 'gboolean', 'blocked': 'gboolean', 'deadline_usec': 'gint64', 'sequence': 'guint64',
        'is_default': 'gboolean', 'watch_only': 'gboolean', 'can_sign': 'gboolean',
        'backup_acknowledged': 'gboolean', 'app_count': 'guint',
        'principal': 'const char *', 'verified_as': 'const char *', 'transport': 'const char *',
        'operation': 'const char *', 'outcome': 'const char *', 'trust': 'const char *',
        'decision': 'const char *', 'relay': 'const char *', 'event_kind': 'gint', 'has_event_kind': 'gboolean',
        'timestamp': 'guint64', 'last_used_at': 'guint64', 'expires_at': 'guint64', 'revision': 'guint64', 'owner_generation': 'guint64',
        'paused': 'gboolean', 'remote_enabled': 'gboolean', 'relays': 'const char *const *', 'methods': 'const char *const *'},
    '_GrottoPanelDescriptor': {'id': 'const char *', 'title': 'const char *', 'icon_name': 'const char *'},
    '_GrottoPanelPermission': {'operation': 'const char *', 'decision': 'const char *', 'event_kind': 'gint',
        'expires_at': 'guint64', 'relay': 'const char *'},
    '_GrottoPanelCommandArgs': {'principal': 'const char *', 'label': 'const char *', 'public_key': 'const char *',
        'trust': 'const char *', 'permissions': 'const GrottoPanelPermission *', 'n_permissions': 'gsize',
        'expected_revision': 'guint64', 'expected_generation': 'guint64', 'remote_enabled': 'gboolean', 'relays': 'const char *const *', 'methods': 'const char *const *'},
    '_GrottoPanelsHost': {'show_toast': 'void (*)(gpointer, const char *)', 'review_request': 'void (*)(gpointer, const char *)',
        'add_identity': 'void (*)(gpointer)', 'add_app': 'void (*)(gpointer)', 'open_backup': 'void (*)(gpointer, const char *)',
        'remove_identity': 'void (*)(gpointer, const char *)', 'replace_identity': 'void (*)(gpointer, const char *)',
        'open_in_signer': 'void (*)(gpointer, const char *)',
        'can_command': 'gboolean (*)(gpointer, GrottoPanelCommand)',
        'command_async': 'void (*)(gpointer, GrottoPanelCommand, const GrottoPanelCommandArgs *, GCancellable *, GAsyncReadyCallback, gpointer)'},
    '_GrottoPanelClass': {'parent_class': 'AdwNavigationPageClass', 'padding': 'gpointer[8]'},
}

def objects(data):
    decoder = json.JSONDecoder()
    while data.strip():
        obj, end = decoder.raw_decode(data.lstrip())
        yield obj
        data = data.lstrip()[end:]

def check_schema(header):
    data = subprocess.check_output([clang, '-x', 'c', '-fsyntax-only', '-Xclang', '-ast-dump=json',
        '-Xclang', '-ast-dump-filter=Grotto', *include, *flags, str(header)], stderr=subprocess.PIPE, text=True)
    declarations = list(objects(data))
    records = {o['name']: o for o in declarations if o.get('kind') == 'RecordDecl' and o.get('completeDefinition') and o.get('name')}
    enums = {o.get('name'): [v['name'] for v in o.get('inner', []) if v.get('kind') == 'EnumConstantDecl']
             for o in declarations if o.get('kind') == 'EnumDecl'}
    assert enums['_GrottoPanelCommand'] == ['GROTTO_PANEL_' + name for name in (
        'SET_CLIENT_LABEL', 'SET_DEFAULT_IDENTITY', 'SET_CLIENT_TRUST', 'SET_PERMISSIONS', 'REVOKE_CLIENT',
        'PAUSE', 'RESUME', 'CLEAR_ACTIVITY', 'CONFIGURE_REMOTE', 'RESET_REMOTE', 'SET_IDENTITY_LABEL', 'N_COMMANDS')], 'closed command enum changed'
    assert enums['_GrottoPanelsModel'] == ['GROTTO_PANELS_' + name for name in ('IDENTITIES', 'APPS', 'REQUESTS', 'ACTIVITY', 'STATUS', 'N_MODELS')]
    assert enums['_GrottoPanelsQuery'] == ['GROTTO_PANELS_QUERY_PERMISSIONS', 'GROTTO_PANELS_QUERY_ACTIVITY']
    assert enums['_GrottoPanelsAccess'] == ['GROTTO_PANELS_ACCESS_' + name for name in ('SESSION', 'MANAGER', 'APPROVER')]
    assert enums['_GrottoPanelHostAction'] == ['GROTTO_PANEL_HOST_' + name for name in (
        'TOAST', 'REVIEW', 'ADD_IDENTITY', 'ADD_APP', 'BACKUP', 'REMOVE', 'REPLACE', 'OPEN_SIGNER', 'N_ACTIONS')]
    for name, expected in schemas.items():
        fields = {f['name']: f['type']['qualType'] for f in records[name]['inner'] if f['kind'] == 'FieldDecl'}
        assert fields == expected, f'complete compiler schema mismatch: {name}: {fields}'
    iface = records['_GrottoPanelsServiceInterface']
    fields = {f['name']: f['type']['qualType'] for f in iface['inner'] if f['kind'] == 'FieldDecl'}
    assert set(fields) == {'parent_iface', 'get_model', 'is_available', 'can', 'invoke_async', 'invoke_finish', 'query_async', 'query_finish', 'padding'}
    assert fields['is_available'] == 'gboolean (*)(GrottoPanelsService *)', fields
    assert fields['can'] == 'gboolean (*)(GrottoPanelsService *, GrottoPanelCommand)', fields
    assert fields['parent_iface'] == 'GTypeInterface' and fields['padding'] == 'gpointer[6]', fields
    assert fields['invoke_async'] == 'void (*)(GrottoPanelsService *, GrottoPanelCommand, const GrottoPanelCommandArgs *, GCancellable *, GAsyncReadyCallback, gpointer)', fields
    assert fields['invoke_finish'] == 'gboolean (*)(GrottoPanelsService *, GAsyncResult *, GError **)', fields
    assert fields['query_async'] == 'void (*)(GrottoPanelsService *, GrottoPanelsQuery, const char *, guint64, GCancellable *, GAsyncReadyCallback, gpointer)', fields
    assert fields['query_finish'] == 'GListModel *(*)(GrottoPanelsService *, GAsyncResult *, GError **)', fields
    assert fields['get_model'] == 'GListModel *(*)(GrottoPanelsService *, GrottoPanelsModel)', fields

exports = {
    'grotto_panels_item_get_type', 'grotto_panels_item_new', 'grotto_panels_item_get_metadata',
    'grotto_panels_metadata_validate', 'grotto_panels_public_text_validate', 'grotto_panel_command_get_access',
    'grotto_panel_command_args_validate', 'grotto_panels_service_get_type', 'grotto_panels_service_get_model',
    'grotto_panels_service_is_available', 'grotto_panels_service_can', 'grotto_panels_service_invoke_async',
    'grotto_panels_service_invoke_finish', 'grotto_panel_get_type', 'grotto_panel_set_service', 'grotto_panel_get_service',
    'grotto_panel_get_cancellable', 'grotto_panel_set_host', 'grotto_panel_host_can', 'grotto_panel_host_invoke',
    'grotto_panel_create_metadata_row', 'grotto_identity_panel_get_type', 'grotto_identity_panel_new',
    'grotto_identity_panel_get_navigation', 'grotto_panels_get_descriptors',
    'grotto_panels_service_query_async', 'grotto_panels_service_query_finish',
    'grotto_panel_can_command', 'grotto_panel_command_async', 'grotto_panel_command_finish',
    'grotto_apps_panel_get_type', 'grotto_apps_panel_new', 'grotto_apps_panel_get_navigation',
    'grotto_apps_panel_filter_identity', 'grotto_apps_panel_set_pending_group', 'grotto_apps_panel_show_app',
    'grotto_activity_panel_get_type', 'grotto_activity_panel_new', 'grotto_activity_panel_filter_app',
    'grotto_remote_panel_get_type', 'grotto_remote_panel_new',
}
# Exact production imports, not a spelling blacklist of individual secret RPCs.
# No D-Bus, sockets, dynamic loading or file I/O dispatch exists in this DSO.
imports = set('''g_free g_strdup g_ascii_strdown g_utf8_validate g_set_error_literal g_error_free
 g_uri_parse g_uri_unref g_uri_get_host g_uri_get_userinfo g_uri_get_query g_uri_get_fragment g_uri_get_scheme g_strcmp0
 g_object_new g_object_ref g_object_unref g_object_notify_by_pspec g_object_class_install_properties
 g_param_spec_object g_value_get_object g_value_set_object g_type_check_instance_cast g_type_check_instance_is_a
 g_type_check_class_cast g_type_class_peek_parent g_type_interface_peek g_type_register_static_simple
 g_type_add_instance_private g_type_interface_add_prerequisite g_type_add_interface_static
 g_once_init_enter g_once_init_leave g_intern_static_string g_return_if_fail_warning g_log
 g_signal_new g_signal_emit g_signal_connect_object g_signal_connect_data g_signal_handler_disconnect
 g_cancellable_new g_cancellable_cancel g_list_model_get_item_type
 g_task_new g_task_set_source_tag g_task_return_error g_task_propagate_boolean g_task_is_valid
 g_async_result_is_tagged g_io_error_quark g_object_get_type g_list_model_get_type
 g_ascii_table g_assertion_message_expr g_once_init_enter_pointer g_once_init_leave_pointer
 g_task_get_name g_task_get_type g_task_set_static_name g_type_class_adjust_private_offset g_type_name
 strlen strstr memset memcpy strcmp __stack_chk_guard __stack_chk_fail __cxa_finalize __gmon_start__
 _ITM_registerTMCloneTable _ITM_deregisterTMCloneTable adw_navigation_page_get_type adw_action_row_new
 adw_preferences_row_get_type adw_preferences_row_set_use_markup adw_preferences_row_set_title
 adw_action_row_get_type adw_action_row_set_subtitle'''.split())

# W3 Identity widget/template/model/clipboard and translation imports only.
# Still no bus, network, filesystem, generic dispatch or secret-store imports.
imports.update('''__cxa_atexit adw_action_row_add_prefix adw_action_row_add_suffix adw_action_row_set_subtitle_lines
 adw_navigation_page_set_child adw_avatar_new adw_avatar_set_text adw_banner_set_button_label adw_navigation_view_get_visible_page adw_navigation_view_push
 adw_status_page_set_description adw_status_page_set_title adw_banner_set_revealed adw_banner_set_title
 g_list_model_get_item g_list_model_get_n_items g_malloc0_n g_object_get_data g_object_set_data_full
 g_static_resource_fini g_static_resource_get_resource g_static_resource_init g_strconcat g_strdup_printf
 g_weak_ref_clear g_weak_ref_get g_weak_ref_init gdk_clipboard_set_text
 gtk_editable_get_text gtk_editable_get_type gtk_editable_set_text gtk_image_new_from_icon_name
 gtk_list_box_append gtk_list_box_remove gtk_list_box_row_get_type gtk_list_box_row_set_activatable
 gtk_stack_set_visible_child_name gtk_widget_class_bind_template_child_full gtk_widget_class_set_template_from_resource
 gtk_widget_dispose_template gtk_widget_get_clipboard gtk_widget_get_first_child gtk_widget_get_type gtk_widget_has_focus
 gdk_display_get_default gtk_icon_theme_add_resource_path gtk_icon_theme_get_for_display
 gtk_widget_init_template gtk_widget_set_sensitive gtk_widget_set_visible gtk_widget_get_state_flags gtk_widget_grab_focus
 libintl_gettext libintl_ngettext gettext ngettext g_dgettext g_dngettext
 bindtextdomain bind_textdomain_codeset libintl_bindtextdomain libintl_bind_textdomain_codeset
 g_param_spec_boolean g_value_get_boolean g_value_set_boolean g_object_bind_property g_object_get g_object_set'''.split())

# W4 management widgets, typed paged reads, owned arrays and async host results.
# This remains a closed symbol list; no IPC, filesystem or transport imports.
imports.update('''adw_action_row_set_activatable_widget adw_action_row_set_subtitle_selectable
 adw_alert_dialog_add_responses adw_alert_dialog_choose adw_alert_dialog_choose_finish adw_alert_dialog_get_type adw_alert_dialog_new
 adw_alert_dialog_set_close_response adw_alert_dialog_set_default_response adw_alert_dialog_set_response_appearance
 adw_avatar_get_type adw_banner_get_type adw_banner_new adw_combo_row_get_selected adw_combo_row_get_type adw_combo_row_new
 gtk_widget_get_root gtk_root_get_focus gtk_root_set_focus gtk_widget_is_ancestor gtk_button_get_child gtk_widget_child_focus gtk_widget_grab_focus
 adw_combo_row_set_enable_search adw_combo_row_set_model adw_combo_row_set_selected adw_dialog_close adw_dialog_new adw_dialog_present adw_dialog_set_child
 adw_dialog_set_content_height adw_dialog_set_content_width adw_dialog_set_title adw_entry_row_get_type adw_entry_row_new
 adw_entry_row_set_show_apply_button adw_header_bar_new adw_navigation_page_new adw_navigation_page_set_title
 adw_navigation_view_pop_to_tag adw_navigation_view_add adw_navigation_view_get_type adw_navigation_view_new adw_preferences_group_add adw_preferences_group_get_type
 adw_preferences_group_new adw_preferences_group_set_description adw_preferences_group_set_title adw_preferences_page_add
 adw_preferences_page_get_type adw_preferences_page_new adw_preferences_page_remove adw_switch_row_get_active adw_switch_row_set_active
 adw_toolbar_view_add_bottom_bar adw_toolbar_view_add_top_bar adw_toolbar_view_get_type adw_toolbar_view_new adw_toolbar_view_set_content
 g_array_append_vals g_array_new g_array_unref g_ascii_strtoll g_cancellable_is_cancelled g_date_time_format
 g_date_time_new_from_unix_local g_date_time_unref g_get_monotonic_time g_get_real_time g_list_append g_list_free
 g_list_store_append g_list_store_new g_list_store_remove_all g_ptr_array_add g_ptr_array_new g_ptr_array_new_with_free_func g_ptr_array_unref
 g_signal_handlers_disconnect_matched g_strdupv g_strfreev g_strjoinv g_strsplit_set g_task_get_source_object g_task_get_task_data
 g_task_propagate_pointer g_task_return_boolean g_task_return_error_if_cancelled g_task_return_new_error g_task_set_task_data
 g_utf8_casefold g_weak_ref_set gtk_accessible_get_type gtk_accessible_update_property gtk_box_append gtk_box_get_type gtk_box_new
 gtk_box_set_homogeneous gtk_button_get_type gtk_button_new_from_icon_name gtk_button_new_with_label gtk_button_set_label
 gtk_check_button_get_active gtk_check_button_get_type gtk_check_button_new gtk_check_button_set_active gtk_check_button_set_group
 gtk_custom_filter_new gtk_filter_changed gtk_filter_get_type gtk_filter_list_model_new gtk_label_get_type gtk_label_new
 gtk_label_set_text gtk_label_set_wrap gtk_label_set_wrap_mode gtk_label_set_xalign gtk_list_box_get_type gtk_list_box_new
 gtk_list_box_set_selection_mode gtk_list_item_get_child gtk_list_item_get_item gtk_list_item_set_child gtk_list_view_set_factory
 gtk_list_view_set_model gtk_no_selection_new gtk_selection_model_get_type gtk_signal_list_item_factory_new gtk_string_list_append
 gtk_string_list_new gtk_widget_add_css_class gtk_widget_get_next_sibling gtk_widget_set_halign gtk_widget_set_margin_bottom
 gtk_widget_set_margin_end gtk_widget_set_margin_start gtk_widget_set_margin_top gtk_widget_set_tooltip_text gtk_widget_set_valign'''.split())

def check_symbols(path):
    output = subprocess.check_output(['nm', '-g', str(path)] if sys.platform == 'darwin' else ['nm', '-D', str(path)], text=True)
    defined, undefined = set(), set()
    for line in output.splitlines():
        parts = line.split()
        if len(parts) < 2 or len(parts[-2]) != 1:
            continue
        name = parts[-1].split('@')[0]
        if sys.platform == 'darwin' and name.startswith('_'):
            name = name[1:]
        if name.startswith(('__asan_', '__ubsan_', '__sanitizer_')):
            continue
        if parts[-2] in {'U', 'w', 'v'}:
            undefined.add(name)
        elif parts[-2] in {'T', 'D', 'B', 'R', 'W', 'V', 'S'}:
            defined.add(name)
    assert undefined <= imports, 'unapproved compiled import: ' + str(sorted(undefined - imports))
    assert defined == exports, 'compiled export mismatch: ' + str(sorted(defined ^ exports))
    if sys.platform.startswith('linux'):
        dynamic = subprocess.check_output(['readelf', '-d', str(path)], text=True)
        needed = re.findall(r'\(NEEDED\).*?\[(.*?)\]', dynamic)
        libs = subprocess.check_output(['pkg-config', '--libs-only-l', 'gtk4', 'libadwaita-1'], text=True).split()
        allowed = {f[2:] for f in libs if f.startswith('-l')} | {'c', 'm', 'gcc_s', 'asan', 'ubsan'}
        assert needed and all(n.startswith('ld-linux-') or (n.startswith('lib') and n[3:].split('.so')[0] in allowed) for n in needed), needed

header = root / 'include/grotto-panels-0/grotto-panels.h'
check_schema(header)
check_symbols(library)
subprocess.run([admission], check=True)
# Counterfactuals from the independent review must fail the ORIGINAL gate.
with tempfile.TemporaryDirectory(prefix='grotto-boundary-mutations-') as temporary:
    directory = pathlib.Path(temporary)
    for name in ('nsec', 'mnemonic', 'pairing_token'):
        mutated = directory / 'mutated.h'
        mutated.write_text(header.read_text().replace('gboolean needs_review;', f'const gchar *{name};\n  gboolean needs_review;'))
        try:
            check_schema(mutated)
        except AssertionError as error:
            assert 'complete compiler schema mismatch' in str(error)
        else:
            raise AssertionError('credential schema mutation escaped enforcement: ' + name)
    mutated.write_text(header.read_text().replace('GROTTO_PANEL_N_COMMANDS', 'GROTTO_PANEL_SENSITIVE_EXPORT, GROTTO_PANEL_N_COMMANDS'))
    try:
        check_schema(mutated)
    except AssertionError as error:
        assert 'closed command enum changed' in str(error)
    else:
        raise AssertionError('sensitive-command enum mutation escaped enforcement')
    nested = directory / 'nested'
    nested.mkdir()
    probe = nested / 'probe.c'
    probe.write_text('#include <gio/gio.h>\n__attribute__((visibility("default"))) void regression_probe(GDBusConnection *bus) {'
        'g_dbus_connection_call(bus,"org.nostr.Signer","/org/nostr/signer/grotto","org.nostr.Grotto.Control1",'
        '"Decide" "Request",NULL,NULL,G_DBUS_CALL_FLAGS_NONE,-1,NULL,NULL,NULL);}\n')
    mutant = directory / ('probe.dylib' if sys.platform == 'darwin' else 'probe.so')
    link = shlex.split(subprocess.check_output(['pkg-config', '--libs', 'gtk4', 'libadwaita-1'], text=True))
    subprocess.run([clang, '-shared', '-fPIC', '-fvisibility=hidden', '-DGROTTO_PANELS_COMPILATION', '-DGETTEXT_PACKAGE="grotto-panels"', '-DGROTTO_PANELS_LOCALEDIR="/usr/share/locale"',
        *include, *flags, *map(str, root.joinpath('src').rglob('*.c')), str(probe), *link, '-o', str(mutant)], check=True)
    try:
        check_symbols(mutant)
    except AssertionError as error:
        assert 'unapproved compiled import' in str(error) and 'g_dbus_connection_call' in str(error)
    else:
        raise AssertionError('adjacent-literal/nested-source RPC mutation escaped enforcement')
print('PASS: complete compiler schema, compiled IPC boundary, credential admission and negative mutations')
