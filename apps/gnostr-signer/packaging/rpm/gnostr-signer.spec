# gnostr-signer.spec - RPM spec file for GNostr Signer
# Targets: Fedora 38+, RHEL 9+, and compatible distributions
#
# Build instructions:
#   1. Install build dependencies:
#      sudo dnf builddep gnostr-signer.spec
#      # or manually:
#      sudo dnf install cmake ninja-build gcc gcc-c++ pkgconfig \
#          gtk4-devel libadwaita-devel libsecret-devel json-glib-devel \
#          libsodium-devel libsecp256k1-devel openssl-devel jansson-devel \
#          libwebsockets-devel nsync-devel glib2-devel p11-kit-devel \
#          desktop-file-utils libappstream-glib systemd-rpm-macros
#
#   2. Create source tarball:
#      tar czf nostrc-%{version}.tar.gz nostrc-%{version}/
#
#   3. Build RPM:
#      rpmbuild -ba gnostr-signer.spec
#
#   4. Install:
#      sudo dnf install ~/rpmbuild/RPMS/x86_64/gnostr-signer-*.rpm
#
# Post-install:
#   - Compile GSettings schemas: glib-compile-schemas /usr/share/glib-2.0/schemas/
#   - Update icon cache: gtk-update-icon-cache /usr/share/icons/hicolor/
#   - Enable daemon (user service): systemctl --user enable --now gnostr-signer-daemon

%global app_id          org.gnostr.Signer
%global dbus_name       org.nostr.Signer

Name:           gnostr-signer
Version:        0.1.0
Release:        1%{?dist}
Summary:        Secure Nostr key management and signing application

License:        MIT
URL:            https://github.com/chebizarro/nostrc
Source0:        %{url}/archive/refs/tags/v%{version}/nostrc-%{version}.tar.gz

# Required for build
BuildRequires:  cmake >= 3.30
BuildRequires:  ninja-build
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig

# GTK4/GNOME stack
BuildRequires:  pkgconfig(gtk4) >= 4.10
BuildRequires:  pkgconfig(libadwaita-1) >= 1.3
BuildRequires:  pkgconfig(glib-2.0) >= 2.74
BuildRequires:  pkgconfig(gobject-2.0)
BuildRequires:  pkgconfig(gio-2.0)
BuildRequires:  pkgconfig(json-glib-1.0)

# Cryptography and security
BuildRequires:  pkgconfig(libsecret-1)
BuildRequires:  pkgconfig(libsodium)
BuildRequires:  pkgconfig(libsecp256k1)
BuildRequires:  pkgconfig(openssl) >= 3.0

# Protocol and data handling
BuildRequires:  pkgconfig(jansson)
BuildRequires:  pkgconfig(libwebsockets)
BuildRequires:  nsync-devel

# Optional: PKCS#11 HSM support
BuildRequires:  pkgconfig(p11-kit-1)

# Desktop integration validation
BuildRequires:  desktop-file-utils
BuildRequires:  libappstream-glib

# Systemd macros for user services
BuildRequires:  systemd-rpm-macros

# GResource compilation
BuildRequires:  glib2-devel
BuildRequires:  /usr/bin/glib-compile-resources
BuildRequires:  /usr/bin/glib-compile-schemas

# Runtime dependencies
Requires:       gtk4 >= 4.10
Requires:       libadwaita >= 1.3
Requires:       libsecret
Requires:       libsodium
Requires:       libsecp256k1
Requires:       openssl-libs >= 3.0
Requires:       jansson
Requires:       libwebsockets
Requires:       nsync
Requires:       dbus

# For icon cache and schema compilation
Requires(post):   glib2
Requires(postun): glib2
Requires(post):   gtk-update-icon-cache
Requires(postun): gtk-update-icon-cache

# Daemon subpackage (can be installed independently)
%package daemon
Summary:        GNostr Signer background daemon
Requires:       %{name} = %{version}-%{release}
Requires:       systemd
%{?systemd_requires}

%description daemon
Background daemon for GNostr Signer providing D-Bus signing service
(NIP-55L) and Unix Domain Socket fallback (NIP-5F). The daemon manages
secure key storage and handles signing requests from Nostr applications.

Enable with: systemctl --user enable --now gnostr-signer-daemon

%description
GNostr Signer is a GTK4/libadwaita application for secure Nostr key
management and event signing. It provides:

- Secure key generation, import, and storage via libsecret (GNOME Keyring)
- NIP-55L D-Bus signing service for desktop Nostr applications
- NIP-5F Unix Domain Socket fallback for non-D-Bus environments
- NIP-46 Nostr Connect (bunker) remote signing support
- NIP-07 browser extension native messaging host
- Permission management per-application
- PKCS#11 HSM support for hardware key storage

The application integrates with the GNOME desktop environment and follows
modern GTK4 design guidelines.

%prep
%autosetup -n nostrc-%{version}

%build
%cmake \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_APPS=ON \
    -DBUILD_NATIVE_HOST=ON \
    -DBUILD_SIGNER_TESTS=OFF \
    -DGNOSTR_SIGNER_WITH_PKCS11=ON \
    %{nil}

%cmake_build --target gnostr-signer gnostr-signer-daemon

%install
# Use cmake --install for proper DESTDIR handling
%cmake_install

# Ensure required directories exist
install -d %{buildroot}%{_bindir}
install -d %{buildroot}%{_datadir}/applications
install -d %{buildroot}%{_datadir}/dbus-1/services
install -d %{buildroot}%{_datadir}/glib-2.0/schemas
install -d %{buildroot}%{_datadir}/icons/hicolor/scalable/apps
install -d %{buildroot}%{_userunitdir}
install -d %{buildroot}%{_datadir}/metainfo

# Install binaries (if not already installed by cmake)
test -f %{buildroot}%{_bindir}/gnostr-signer || \
    install -m 0755 %{_vpath_builddir}/apps/gnostr-signer/gnostr-signer %{buildroot}%{_bindir}/
test -f %{buildroot}%{_bindir}/gnostr-signer-daemon || \
    install -m 0755 %{_vpath_builddir}/apps/gnostr-signer/gnostr-signer-daemon %{buildroot}%{_bindir}/

# Install desktop file
desktop-file-install \
    --dir=%{buildroot}%{_datadir}/applications \
    apps/gnostr-signer/packaging/appimage/gnostr-signer.desktop

# Install D-Bus service file (process template if needed)
sed 's|@CMAKE_INSTALL_FULL_BINDIR@|%{_bindir}|g' \
    apps/gnostr-signer/data/org.nostr.Signer.service > \
    %{buildroot}%{_datadir}/dbus-1/services/%{dbus_name}.service

# Install systemd user unit
install -m 0644 apps/gnostr-signer/daemon/packaging/systemd/user/gnostr-signer-daemon.service \
    %{buildroot}%{_userunitdir}/

# Install GSettings schema
install -m 0644 apps/gnostr-signer/data/org.gnostr.Signer.gschema.xml \
    %{buildroot}%{_datadir}/glib-2.0/schemas/

# Install AppStream metadata
install -m 0644 apps/gnostr-signer/data/org.nostr.Signer.appdata.xml \
    %{buildroot}%{_datadir}/metainfo/%{app_id}.metainfo.xml

# Install icon
install -m 0644 apps/gnostr-signer/data/icons/hicolor/scalable/apps/%{app_id}.svg \
    %{buildroot}%{_datadir}/icons/hicolor/scalable/apps/

%check
# Validate desktop file
desktop-file-validate %{buildroot}%{_datadir}/applications/gnostr-signer.desktop

# Validate AppStream metadata
appstream-util validate-relax --nonet \
    %{buildroot}%{_datadir}/metainfo/%{app_id}.metainfo.xml || :

%post
# Compile GSettings schemas
%{_bindir}/glib-compile-schemas %{_datadir}/glib-2.0/schemas &>/dev/null || :

# Update icon cache
touch --no-create %{_datadir}/icons/hicolor &>/dev/null || :
if [ -x %{_bindir}/gtk-update-icon-cache ]; then
    %{_bindir}/gtk-update-icon-cache --quiet %{_datadir}/icons/hicolor &>/dev/null || :
fi

%postun
# Recompile schemas on uninstall
%{_bindir}/glib-compile-schemas %{_datadir}/glib-2.0/schemas &>/dev/null || :

# Update icon cache
touch --no-create %{_datadir}/icons/hicolor &>/dev/null || :
if [ -x %{_bindir}/gtk-update-icon-cache ]; then
    %{_bindir}/gtk-update-icon-cache --quiet %{_datadir}/icons/hicolor &>/dev/null || :
fi

%post daemon
%systemd_user_post gnostr-signer-daemon.service

%preun daemon
%systemd_user_preun gnostr-signer-daemon.service

%postun daemon
%systemd_user_postun_with_restart gnostr-signer-daemon.service

%files
%license LICENSE
%doc README.md
%doc apps/gnostr-signer/ARCHITECTURE.md
%{_bindir}/gnostr-signer
%{_datadir}/applications/gnostr-signer.desktop
%{_datadir}/glib-2.0/schemas/org.gnostr.Signer.gschema.xml
%{_datadir}/icons/hicolor/scalable/apps/%{app_id}.svg
%{_datadir}/metainfo/%{app_id}.metainfo.xml

%files daemon
%{_bindir}/gnostr-signer-daemon
%{_datadir}/dbus-1/services/%{dbus_name}.service
%{_userunitdir}/gnostr-signer-daemon.service

%changelog
* Thu Jan 23 2025 GNostr Maintainers <maintainers@gnostr.org> - 0.1.0-1
- Initial RPM packaging for Fedora 38+ and RHEL 9+
- GTK4/libadwaita UI application
- D-Bus signing service (NIP-55L)
- Unix Domain Socket fallback (NIP-5F)
- NIP-46 bunker support
- PKCS#11 HSM support
- Systemd user service integration
