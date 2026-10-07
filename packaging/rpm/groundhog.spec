# groundhog.spec: Groundhog and its signer, Grotto, from the nostrc monorepo
# (W30, nostrc-bgq8). Fedora 40+.
#
# Source0 is the archive written by scripts/make-desktop-source-archive.sh:
# it includes the nostrdb and nsync submodules, which GitHub's tag archives
# leave out. To build from a checkout:
#
#   scripts/make-desktop-source-archive.sh 0.12.0-alpha2 ~/rpmbuild/SOURCES
#   rpmbuild -ba packaging/rpm/groundhog.spec
#
# nsync: Fedora has no nsync package (nostrc-dd5y); the copy in third_party/
# is linked statically and declared as bundled. `--with system_nsync` uses a
# packaged one instead.

%global upstream_version 0.12.0-alpha2
%bcond_with system_nsync

Name:           groundhog
Version:        0.12.0
Release:        0.2.alpha2%{?dist}
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

Requires:       grotto = %{version}-%{release}
Requires:       hicolor-icon-theme
Requires:       gstreamer1-plugins-base
Requires:       gstreamer1-plugins-good
Recommends:     gnome-keyring
Recommends:     gstreamer1-plugins-bad-free
Recommends:     webp-pixbuf-loader
Suggests:       tor

%description
Groundhog is a GTK4 and libadwaita messenger for Nostr: private messages
(NIP-17) with one person or a small group, relay groups (NIP-29) and
end-to-end encrypted Marmot groups that interoperate with White Noise.

Messages are kept only on this device, in an encrypted database. Your
private key never enters Groundhog: it stays in Grotto, which asks you
before anything is signed or unlocked. There are no read receipts, typing
indicators or online status, and nothing loads from the web unless you ask.

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
encrypts and decrypts on behalf of Nostr apps over the session bus
(NIP-55L, org.nostr.Signer). Each app is identified, and you approve what
it may do; private keys never leave Grotto.

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
