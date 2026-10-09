# groundhog.spec: Groundhog and its signer, Grotto, from the nostrc monorepo
# (W30, nostrc-bgq8). Fedora 40+.
#
# Source0 is the archive written by scripts/make-desktop-source-archive.sh:
# it includes the nostrdb and nsync submodules, which GitHub's tag archives
# leave out. To build from a checkout:
#
#   scripts/make-desktop-source-archive.sh 0.12.0-alpha5 ~/rpmbuild/SOURCES
#   rpmbuild -ba packaging/rpm/groundhog.spec
#
# nsync: Fedora has no nsync package (nostrc-dd5y); the copy in third_party/
# is linked statically and declared as bundled. `--with system_nsync` uses a
# packaged one instead.

%global upstream_version 0.12.0-alpha5
%bcond_with system_nsync

Name:           groundhog
Version:        0.12.0
Release:        0.5.alpha5%{?dist}
Summary:        Private messaging on Nostr for GNOME (alpha)
License:        MIT
URL:            https://github.com/chebizarro/nostrc
Source0:        %{url}/releases/download/groundhog-v%{upstream_version}/groundhog-%{upstream_version}.tar.gz

BuildRequires:  cmake >= 3.22
BuildRequires:  ninja-build
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig
BuildRequires:  python3
BuildRequires:  pkgconfig(glib-2.0) >= 2.76
BuildRequires:  pkgconfig(gtk4) >= 4.14
BuildRequires:  pkgconfig(libadwaita-1) >= 1.5
BuildRequires:  pkgconfig(json-glib-1.0)
BuildRequires:  pkgconfig(libsoup-3.0)
BuildRequires:  pkgconfig(libsecret-1)
BuildRequires:  pkgconfig(sqlcipher)
BuildRequires:  pkgconfig(libxml-2.0)
BuildRequires:  pkgconfig(gstreamer-1.0) >= 1.20
BuildRequires:  pkgconfig(gstreamer-app-1.0)
BuildRequires:  pkgconfig(openssl) >= 3.0
BuildRequires:  pkgconfig(libsecp256k1)
BuildRequires:  pkgconfig(libsodium)
BuildRequires:  pkgconfig(jansson)
BuildRequires:  pkgconfig(libwebsockets)
BuildRequires:  pkgconfig(libcurl)
BuildRequires:  pkgconfig(libqrencode)
BuildRequires:  pkgconfig(gdk-pixbuf-2.0)
BuildRequires:  desktop-file-utils
BuildRequires:  libappstream-glib
BuildRequires:  systemd-rpm-macros
%if %{with system_nsync}
BuildRequires:  nsync-devel
%else
Provides:       bundled(nsync) = 1.29.2
%endif

Requires:       qrencode-libs
Requires:       hicolor-icon-theme
Requires:       gstreamer1-plugins-base
Requires:       gstreamer1-plugins-good
Recommends:     gnome-keyring
Recommends:     gstreamer1-plugins-bad-free
Recommends:     webp-pixbuf-loader
Suggests:       grotto = %{version}-%{release}
Suggests:       tor

%description
Groundhog is a GTK4 and libadwaita messenger for Nostr: private messages
(NIP-17) with one person or a small group, relay groups (NIP-29) and
end-to-end encrypted Marmot groups that interoperate with White Noise.

Messages are kept only on this device, in an encrypted database. Your
private key never enters Groundhog: use Grotto for a local account or pair
a NIP-46 remote signer (phone or bunker). Grotto is not needed for
remote-signer-only accounts. There are no read receipts, typing indicators
or online status, and nothing loads from the web unless you ask.

This is an alpha release.

%package -n grotto
Summary:        Keeps your Nostr keys and signs for your apps (preview)
Requires:       hicolor-icon-theme
Recommends:     gnome-keyring
%if %{without system_nsync}
Provides:       bundled(nsync) = 1.29.2
%endif

%description -n grotto
Grotto holds your Nostr identities in the system keyring and signs,
encrypts and decrypts for every Nostr app that speaks NIP-55L (local apps
over the session bus, org.nostr.Signer) or NIP-46 (Nostr Connect, remote
apps over relays). One Grotto serves them all, Groundhog included. Each
app is identified, and you approve what it may do; private keys never
leave Grotto.

This package contains the Grotto app (approvals, identities, settings) and
its background service, which starts on demand when an app asks.

Grotto is a preview: its interface is still being reworked.

%prep
%autosetup -n groundhog-%{upstream_version}

%build
# BUILD_TESTING must stay OFF: a testing build carries a test-only D-Bus
# interface and refuses to install.
# Fedora's cmake macro sets BUILD_SHARED_LIBS=ON; the apps link the in-tree
# libraries statically (only libnostr-json is shared, installed privately).
%cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_GROUNDHOG=ON \
    -DGROUNDHOG_PACKAGE_CHANNEL=rpm \
    -DBUILD_APPS=ON \
    -DBUILD_GNOSTR_APP=OFF \
    -DBUILD_NATIVE_HOST=OFF \
    -DBUILD_SIGNER_TESTS=OFF \
    -DGROTTO_WITH_PKCS11=OFF \
    -DGROTTO_WITH_HW_WALLET=OFF \
    -DBUILD_RELAYD=OFF \
    -DBUILD_LIBHANAMI=OFF \
    -DSIGNET_ENABLE=OFF \
    -DBUILD_TESTING=OFF \
    -DBUILD_TESTING_FRAMEWORK=OFF \
%if %{with system_nsync}
    -DNOSTR_USE_SYSTEM_NSYNC=ON
%else
    -DNOSTR_USE_SYSTEM_NSYNC=OFF
%endif
%cmake_build --target gnome/groundhog/all apps/grotto/all

%install
DESTDIR=%{buildroot} cmake --install %{__cmake_builddir}/gnome/groundhog
DESTDIR=%{buildroot} cmake --install %{__cmake_builddir}/apps/grotto
# The build tree installs the user unit under %%{_datadir}/systemd/user
# (where systemd also looks); Fedora keeps units in %%{_userunitdir}.
install -d %{buildroot}%{_userunitdir}
mv %{buildroot}%{_datadir}/systemd/user/grotto-daemon.service %{buildroot}%{_userunitdir}/

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/org.nostr.Groundhog.desktop
desktop-file-validate %{buildroot}%{_datadir}/applications/org.nostr.Grotto.desktop
appstream-util validate-relax --nonet %{buildroot}%{_metainfodir}/org.nostr.Groundhog.metainfo.xml

%files
%license LICENSE gnome/groundhog/data/icons/COPYING
%{_bindir}/groundhog
%dir %{_libdir}/groundhog
%{_libdir}/groundhog/libnostr-json.so.1
%{_datadir}/applications/org.nostr.Groundhog.desktop
%{_datadir}/dbus-1/services/org.nostr.Groundhog.service
%{_datadir}/glib-2.0/schemas/org.nostr.Groundhog.gschema.xml
%{_datadir}/icons/hicolor/*/apps/org.nostr.Groundhog.png
%{_metainfodir}/org.nostr.Groundhog.metainfo.xml

%files -n grotto
%license LICENSE
%{_bindir}/grotto
%{_bindir}/grotto-daemon
%dir %{_libdir}/grotto
%{_libdir}/grotto/libnostr-json.so.1
%{_datadir}/applications/org.nostr.Grotto.desktop
%{_datadir}/dbus-1/services/org.nostr.Grotto.service
%{_datadir}/dbus-1/services/org.nostr.Signer.service
%{_datadir}/glib-2.0/schemas/org.nostr.Grotto.gschema.xml
%{_datadir}/icons/hicolor/*/apps/org.nostr.Grotto.png
%{_metainfodir}/org.nostr.Grotto.metainfo.xml
%{_userunitdir}/grotto-daemon.service

%changelog
* Thu Oct 08 2026 Biz <chebizarro@protonmail.com> - 0.12.0-0.5.alpha5
- Faster conversation opening: about half the time for long histories.
- Polls: your own polls show immediately, options as radio buttons or
  check boxes, votes update results instead of adding bubbles.
- Account switcher with display names and profile pictures; copy
  buttons for your npub in Preferences and the QR dialog.
- Earlier messages button only near the top of the history; consistent
  order of reactions, time and receipt on both sides.

* Thu Oct 08 2026 Biz <chebizarro@protonmail.com> - 0.12.0-0.4.alpha4
- Remote signers (NIP-46): pair Amber by scanning a QR code or paste a
  bunker:// link (nsec.app and others); per-account choice of Grotto or a
  remote signer; pairing keys in Secret Service/Keychain; auth_url login
  pages open in the browser; works over Tor. Remote accounts require the
  encrypted message store, so each message needs the phone only once.
- Polls in every conversation type (NIP-17, NIP-29, Marmot) and
  @-mentions with autocomplete.
- Fixes from the alpha 3 test drive: tighter conversation list, reactions
  beside the React button and in Marmot groups, Earlier messages button,
  NIP-29 history, search, relay editing in Preferences, account name and
  picture in Preferences, npub QR codes, inline audio, automatic display
  of encrypted images when enabled.
- Grotto is needed only for Grotto-backed accounts; libqrencode is a new
  dependency.

* Wed Oct 07 2026 Biz <chebizarro@protonmail.com> - 0.12.0-0.3.alpha3
- NIP-04 (kind 4) direct messages from older clients shown read-only,
  marked less private; replies stay NIP-17.
- Fixes from the alpha 2 test drive: profile pictures behind redirects
  (Blossom), display names instead of npubs everywhere, own picture
  without a consent prompt, round avatars beside the bubble, emoji
  picker anchored to the react button, message list padding, composer
  attach and voice buttons beside send.

* Wed Oct 07 2026 Biz <chebizarro@protonmail.com> - 0.12.0-0.2.alpha2
- Grotto security hardening: CSPRNG IPC auth token, daemon IPC
  authentication and named-pipe framing, secure delete without a shell,
  bounded hardware-wallet (Ledger/HID) reads, delegation and approval
  checks, signal-safe shutdown, locale handling after start-up.
- Groundhog hardening from a static review (store array growth, portal
  lifetime, instance paths, entropy fallback).
- Fixes from the alpha 1 test drive: key import and profile creation in
  the installed Grotto, the daemon replacing itself after an upgrade,
  profile pictures by contact everywhere (WebP too), reactions on the
  bubble's side, sidebar named after the profile, app indicator on
  Ubuntu and KDE, one deb per Debian/Ubuntu release.

* Mon Oct 05 2026 Biz <chebizarro@protonmail.com> - 0.12.0-0.1.alpha1
- First alpha: private messages (NIP-17), relay groups (NIP-29) and Marmot
  encrypted groups, with Grotto as the signer.
