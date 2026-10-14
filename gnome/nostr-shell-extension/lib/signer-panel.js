// SPDX-License-Identifier: MIT
import Clutter from 'gi://Clutter';
import Gio from 'gi://Gio';
import GObject from 'gi://GObject';
import St from 'gi://St';

import * as PanelMenu from 'resource:///org/gnome/shell/ui/panelMenu.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import {gettext as _, ngettext} from 'resource:///org/gnome/shell/extensions/extension.js';

import {SignerMonitor} from './signer.js';

export const SignerIndicator = GObject.registerClass(
class SignerIndicator extends PanelMenu.Button {
    constructor(extension) {
        super(0.0, _('Grotto'), false);
        this._icons = {};
        for (const state of ['ready', 'waiting', 'paused', 'unavailable']) {
            this._icons[state] = new Gio.FileIcon({file: extension.dir.get_child('icons')
                .get_child(`nostr-signer-${state}-symbolic.svg`)});
        }
        const box = new St.BoxLayout({style_class: 'panel-status-menu-box'});
        this._icon = new St.Icon({style_class: 'system-status-icon'});
        this._badge = new St.Label({y_align: Clutter.ActorAlign.CENTER});
        box.add_child(this._icon);
        box.add_child(this._badge);
        this.add_child(box);

        this._statusItem = new PopupMenu.PopupMenuItem('', {reactive: false});
        this.menu.addMenuItem(this._statusItem);
        this._bunkerItem = new PopupMenu.PopupMenuItem('', {reactive: false});
        this.menu.addMenuItem(this._bunkerItem);
        this._errorItem = new PopupMenu.PopupMenuItem(_('Action failed — open Grotto to try again'),
            {reactive: false});
        this.menu.addMenuItem(this._errorItem);
        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
        this._reviewItem = this.menu.addAction(_('Review requests'), () => this._monitor.review());
        this._openItem = this.menu.addAction(_('Open Grotto'), () => this._monitor.open());
        this._pauseItem = this.menu.addAction(_('Pause signing'), () => this._monitor.pause());
        this._resumeItem = this.menu.addAction(_('Resume signing'), () => this._monitor.resume());
        this._monitor = new SignerMonitor(state => this._render(state));
        this._menuId = this.menu.connect('open-state-changed', (menu, open) => {
            if (open)
                this._monitor.refresh();
        });
        this._render(this._monitor.state);
    }

    start() {
        // Own the suppression name only after addToStatusArea succeeds.
        this._monitor.start();
    }

    _render(state) {
        const waiting = ngettext('%d request waiting', '%d requests waiting', state.pendingCount)
            .replace('%d', String(state.pendingCount));
        const bunkerLabel = state.bunkerState === 'DEGRADED' ? _('Bunker degraded') :
            state.bunkerState === 'OFFLINE' ? _('Bunker offline') :
            state.bunkerState === 'STARTING' ? _('Bunker starting…') :
            state.bunkerState === 'STOPPING' ? _('Bunker stopping…') : _('Bunker unavailable');
        const labels = {
            stopped: _('Signer not running'), unavailable: _('Signer status unavailable — update or open Grotto'),
            ready: _('Ready'), waiting, paused: _('Signing paused'),
            bunker: bunkerLabel,
        };
        const label = labels[state.phase];
        this._statusItem.label.text = label;
        this.accessible_name = `${_('Grotto')}: ${label}`;
        const iconState = state.phase === 'ready' ? 'ready' : state.phase === 'waiting' ? 'waiting' :
            state.phase === 'paused' ? 'paused' : 'unavailable';
        this._icon.gicon = this._icons[iconState];
        this._badge.text = state.badge;
        this._badge.visible = state.pendingCount > 0;
        this._reviewItem.label.text = `${_('Review requests')} (${state.pendingCount})`;
        this._reviewItem.setSensitive(state.pendingCount > 0 && !state.busy);
        this._openItem.setSensitive(!state.busy);
        this._pauseItem.visible = state.running && !state.paused;
        this._pauseItem.setSensitive(state.canPause);
        this._resumeItem.visible = state.paused;
        this._resumeItem.setSensitive(!state.busy);
        this._bunkerItem.visible = state.bunkerProblem && state.phase !== 'bunker';
        this._bunkerItem.label.text = bunkerLabel;
        this._errorItem.visible = !!state.actionError;
    }

    destroy() {
        this._monitor?.destroy();
        this._monitor = null;
        if (this._menuId)
            this.menu.disconnect(this._menuId);
        this._menuId = 0;
        this._icons = null;
        super.destroy();
    }
});
