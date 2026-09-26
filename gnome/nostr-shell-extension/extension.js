// SPDX-License-Identifier: MIT
//
// Nostr — GNOME Shell extension (bead nostrc-33f1).
//
// A "Nostr Relay" Quick Settings toggle for the per-user session relay with
// the same on/off semantics as Nostr Settings (org.nostr.Settings), live
// statistics from org.nostr.SessionRelay1, message-notification status from
// nostr-notify.service, the Nostr Wallet Connect balance from
// org.nostr.Wallet1 and a "Nostr Settings" shortcut.
//
// Everything is created in enable() and destroyed in disable(): the
// indicator, the toggle, both monitors (D-Bus subscriptions, name watches,
// proxies, timeouts, the file monitor) and every signal connection.

import GObject from 'gi://GObject';
import Gio from 'gi://Gio';

import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import {QuickMenuToggle, SystemIndicator} from 'resource:///org/gnome/shell/ui/quickSettings.js';
import {Extension, gettext as _} from 'resource:///org/gnome/shell/extensions/extension.js';

import {RelayMonitor} from './lib/relay.js';
import {WalletMonitor} from './lib/wallet.js';
import {dmView, relayView, setTranslator, walletView} from './lib/state.js';

const NostrRelayToggle = GObject.registerClass(
class NostrRelayToggle extends QuickMenuToggle {
    constructor(gicon) {
        super({
            title: _('Nostr Relay'),
            subtitle: '',
            gicon,
            toggleMode: false,
        });
        this._gicon = gicon;
        this.menu.setHeader(gicon, _('Nostr Relay'), '');

        this._detailItem = new PopupMenu.PopupMenuItem('', {reactive: false});
        this.menu.addMenuItem(this._detailItem);

        this._infoSeparator = new PopupMenu.PopupSeparatorMenuItem();
        this.menu.addMenuItem(this._infoSeparator);

        this._dmItem = new PopupMenu.PopupMenuItem('', {reactive: false});
        this.menu.addMenuItem(this._dmItem);

        this._walletItem = new PopupMenu.PopupMenuItem('', {reactive: false});
        this.menu.addMenuItem(this._walletItem);

        // Only way the wallet agent is asked from the panel: the user clicks.
        this.grantItem = new PopupMenu.PopupMenuItem(_('Allow balance access…'));
        this.menu.addMenuItem(this.grantItem);

        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
        this.menu.addSettingsAction(_('Nostr Settings'), 'org.nostr.Settings.desktop');
    }

    render(relay, dm, wallet) {
        this.checked = relay.checked;
        this.subtitle = relay.subtitle;
        this.menu.setHeader(this._gicon, _('Nostr Relay'), relay.statusLine);

        this._detailItem.label.text = relay.detailLine ?? '';
        this._detailItem.visible = !!relay.detailLine;

        this._dmItem.label.text = dm ?? '';
        this._dmItem.visible = !!dm;

        this._walletItem.label.text = wallet.label ?? '';
        this._walletItem.visible = !!wallet.label;
        this.grantItem.visible = wallet.canGrant;

        this._infoSeparator.visible = this._detailItem.visible &&
            (this._dmItem.visible || this._walletItem.visible);
    }
});

const NostrIndicator = GObject.registerClass(
class NostrIndicator extends SystemIndicator {
    constructor(extension, settings) {
        super();
        this._settings = settings;

        const gicon = Gio.icon_new_for_string(
            `${extension.path}/icons/nostr-relay-symbolic.svg`);
        this._icon = this._addIndicator();
        this._icon.gicon = gicon;
        this._icon.visible = false;

        this._toggle = new NostrRelayToggle(gicon);
        this.quickSettingsItems.push(this._toggle);

        this._relay = new RelayMonitor(() => this._sync());
        this._wallet = new WalletMonitor(() => this._sync());

        this._toggleClickedId = this._toggle.connect('clicked', () => {
            const view = relayView(this._relay.state);
            if (view.reactive)
                this._relay.setEnabled(!view.checked).catch(logError);
        });
        this._grantActivateId = this._toggle.grantItem.connect('activate',
            () => this._wallet.requestAccess());

        this._settingsIds = [
            settings.connect('changed::refresh-interval', () =>
                this._relay.setRefreshInterval(settings.get_uint('refresh-interval'))),
            settings.connect('changed::show-balance', () =>
                this._wallet.setShowBalance(settings.get_boolean('show-balance'))),
            settings.connect('changed::show-dm-status', () => this._sync()),
        ];

        // One-shot refresh whenever Quick Settings opens (not a poll).
        this._quickSettingsMenu = Main.panel.statusArea.quickSettings.menu;
        this._menuOpenId = this._quickSettingsMenu.connect('open-state-changed', (m, open) => {
            if (!open)
                return;
            this._relay.refresh();
            this._wallet.refresh(settings.get_uint('refresh-interval'));
        });

        this._relay.start(settings.get_uint('refresh-interval'));
        this._wallet.start({showBalance: settings.get_boolean('show-balance')});
        this._sync();
    }

    _sync() {
        if (!this._relay || !this._wallet)
            return;
        const relay = relayView(this._relay.state);
        const dm = this._settings.get_boolean('show-dm-status')
            ? dmView(this._relay.state) : null;
        const wallet = walletView(this._wallet.state);
        this._icon.visible = relay.showIndicator;
        this._toggle.render(relay, dm, wallet);
    }

    destroy() {
        // Handlers first, so nothing can reach a monitor being torn down.
        if (this._toggle) {
            this._toggle.disconnect(this._toggleClickedId);
            this._toggle.grantItem.disconnect(this._grantActivateId);
        }
        if (this._menuOpenId) {
            this._quickSettingsMenu.disconnect(this._menuOpenId);
            this._menuOpenId = 0;
        }
        this._quickSettingsMenu = null;
        for (const id of this._settingsIds)
            this._settings.disconnect(id);
        this._settingsIds = [];
        this._settings = null;

        this._relay?.destroy();
        this._relay = null;
        this._wallet?.destroy();
        this._wallet = null;

        // Destroying the indicator also removes it from Quick Settings.
        this.quickSettingsItems.forEach(item => item.destroy());
        this.quickSettingsItems = [];
        this._toggle = null;

        super.destroy();
    }
});

export default class NostrExtension extends Extension {
    enable() {
        setTranslator(_);
        this._settings = this.getSettings();
        this._indicator = new NostrIndicator(this, this._settings);
        Main.panel.statusArea.quickSettings.addExternalIndicator(this._indicator);
    }

    disable() {
        this._indicator?.destroy();
        this._indicator = null;
        this._settings = null;
        setTranslator(null);
    }
}
