// SPDX-License-Identifier: MIT
//
// Preferences for the Nostr Shell extension (libadwaita).

import Adw from 'gi://Adw';
import Gio from 'gi://Gio';
import Gtk from 'gi://Gtk';

import {ExtensionPreferences, gettext as _} from 'resource:///org/gnome/Shell/Extensions/js/extensions/prefs.js';

export default class NostrPreferences extends ExtensionPreferences {
    fillPreferencesWindow(window) {
        const settings = this.getSettings();
        // Keep the GSettings object alive as long as the window.
        window._nostrSettings = settings;

        const page = new Adw.PreferencesPage({
            title: _('Nostr'),
            icon_name: 'preferences-system-symbolic',
        });

        const relay = new Adw.PreferencesGroup({
            title: _('Session relay'),
            description: _('The toggle follows systemd and the relay’s own statistics. ' +
                'Checking never starts a stopped relay.'),
        });
        const interval = new Adw.SpinRow({
            title: _('Refresh interval'),
            subtitle: _('Seconds between background status checks (30–3600)'),
            adjustment: new Gtk.Adjustment({
                lower: 30, upper: 3600, step_increment: 10, page_increment: 60,
            }),
        });
        settings.bind('refresh-interval', interval, 'value', Gio.SettingsBindFlags.DEFAULT);
        relay.add(interval);
        page.add(relay);

        const menu = new Adw.PreferencesGroup({title: _('Menu')});
        const balance = new Adw.SwitchRow({
            title: _('Show wallet balance'),
            subtitle: _('Read from Nostr Wallet only after you allow it; the panel never asks by itself'),
        });
        settings.bind('show-balance', balance, 'active', Gio.SettingsBindFlags.DEFAULT);
        menu.add(balance);

        const dm = new Adw.SwitchRow({
            title: _('Show message notification status'),
            subtitle: _('Whether nostr-notify is delivering direct-message notifications'),
        });
        settings.bind('show-dm-status', dm, 'active', Gio.SettingsBindFlags.DEFAULT);
        menu.add(dm);
        page.add(menu);

        window.add(page);
    }
}
