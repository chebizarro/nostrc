# Native Grotto Preview; build from a complete checkout/archive with submodules.
# Requires GTK >=4.14, libadwaita >=1.5, GLib >=2.80 (not RHEL 9/Fedora 38).
%global app_id org.nostr.Grotto
Name:           grotto
Version:        0.1.0
Release:        1%{?dist}
Summary:        Nostr identity management and signing service (Preview)
License:        MIT
URL:            https://github.com/chebizarro/nostrc
# Local source archive, including third_party submodules; no fictitious release tag.
Source0:        grotto-%{version}.tar.gz
BuildRequires:  cmake >= 3.22
BuildRequires:  ninja-build
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  python3
BuildRequires:  gettext
BuildRequires:  pkgconfig(gtk4) >= 4.14
BuildRequires:  pkgconfig(libadwaita-1) >= 1.5
BuildRequires:  pkgconfig(glib-2.0) >= 2.80
BuildRequires:  pkgconfig(json-glib-1.0)
BuildRequires:  pkgconfig(libsecret-1)
BuildRequires:  pkgconfig(libsodium)
BuildRequires:  pkgconfig(libsecp256k1)
BuildRequires:  pkgconfig(openssl) >= 3.0
BuildRequires:  pkgconfig(jansson)
BuildRequires:  pkgconfig(libwebsockets)
BuildRequires:  pkgconfig(libqrencode)
BuildRequires:  pkgconfig(zbar)
BuildRequires:  pkgconfig(gstreamer-1.0)
BuildRequires:  pkgconfig(gstreamer-video-1.0)
BuildRequires:  pkgconfig(gstreamer-app-1.0)
# Complete source archive carries third_party/nsync (no distro nsync required).
Provides:       bundled(nsync)
BuildRequires:  desktop-file-utils
BuildRequires:  appstream
BuildRequires:  systemd-rpm-macros
Requires:       gtk4 >= 4.14
Requires:       libadwaita >= 1.5
Requires:       glib2 >= 2.80
Requires:       gstreamer1-plugins-base
Requires:       gstreamer1-plugins-good
Recommends:     gnome-keyring
Recommends:     gnome-shell-extension-nostr
%{?systemd_requires}

%description
One package containing the Grotto GUI and D-Bus-activated daemon. Scoped
permissions govern local NIP-55L and explicitly paired NIP-46 clients.
Client applications do not receive private keys; explicit Grotto key-management
operations may handle them. Signing is not publication. Grotto is not a security
boundary against malware running as the same user. Preview acceptance is pending.

%prep
%autosetup -n grotto-%{version}

%build
%cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_APPS=ON -DBUILD_GNOSTR_APP=OFF -DBUILD_GROUNDHOG=OFF \
    -DBUILD_TESTING=OFF -DBUILD_TESTING_FRAMEWORK=OFF \
    -DBUILD_SIGNER_TESTS=OFF -DBUILD_NATIVE_HOST=ON \
    -DGROTTO_WITH_PKCS11=OFF -DGROTTO_WITH_HW_WALLET=OFF \
    -DBUILD_RELAYD=OFF -DSIGNET_ENABLE=OFF -DBUILD_LIBHANAMI=OFF \
    -DNOSTR_USE_SYSTEM_NSYNC=OFF
%cmake_build --target apps/grotto/all

%install
DESTDIR=%{buildroot} cmake --install %{_vpath_builddir}/apps/grotto
# CMake's canonical user unit installs under share/systemd/user.
install -Dm644 %{buildroot}%{_datadir}/systemd/user/grotto-daemon.service \
    %{buildroot}%{_userunitdir}/grotto-daemon.service
if [ '%{_datadir}/systemd/user' != '%{_userunitdir}' ]; then
    rm %{buildroot}%{_datadir}/systemd/user/grotto-daemon.service
fi

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/%{app_id}.desktop
appstreamcli validate --no-net %{buildroot}%{_datadir}/metainfo/%{app_id}.metainfo.xml

%post
# D-Bus activates the daemon on demand; do not enable login startup here.
%{_bindir}/glib-compile-schemas %{_datadir}/glib-2.0/schemas

%postun
%{_bindir}/glib-compile-schemas %{_datadir}/glib-2.0/schemas

%files
%license LICENSE
%doc apps/grotto/README.md apps/grotto/DAEMON_DEPLOYMENT.md
%{_bindir}/grotto
%{_bindir}/grotto-daemon
%{_libdir}/grotto/
%{_datadir}/applications/%{app_id}.desktop
%{_datadir}/dbus-1/services/%{app_id}.service
%{_datadir}/dbus-1/services/org.nostr.Signer.service
%{_datadir}/dbus-1/interfaces/%{app_id}.Control1.xml
%{_datadir}/glib-2.0/schemas/%{app_id}.gschema.xml
%{_datadir}/icons/hicolor/*/apps/%{app_id}*
%{_datadir}/metainfo/%{app_id}.metainfo.xml
%{_datadir}/grotto/
%{_datadir}/doc/grotto/examples/
%{_userunitdir}/grotto-daemon.service
%{_libexecdir}/nostr-signer-webext-host
%{_datadir}/nostr-signer-webext-host/
%{_datadir}/doc/nostr-signer-webext-host/

%changelog
* Wed Oct 14 2026 Grotto Maintainers - 0.1.0-1
- Fold native GUI/daemon packaging correction into unreleased Preview 0.1.0
- Use canonical desktop/metainfo, activation, autostart template and network examples
