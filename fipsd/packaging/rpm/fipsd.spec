# fipsd.spec: the nostrc C port of the FIPS mesh daemon, for Fedora 42+.
#
# The counterpart of fipsd/packaging/debian: the same files land in the same
# places, the same fips group is created, the same config is seeded and the
# same two units are enabled. Where the two package managers differ, the
# differences are marked rather than smoothed over.
#
# Source0 is the archive written by scripts/make-desktop-source-archive.sh
# with the fipsd name: it carries the nostrdb and nsync submodules, which
# GitHub's tag archives leave out. To build from a checkout:
#
#   scripts/make-desktop-source-archive.sh 0.1.0 ~/rpmbuild/SOURCES fipsd
#   rpmbuild -ba fipsd/packaging/rpm/fipsd.spec
#
# The version is the one fipsd/CMakeLists.txt declares
# (fipsd/packaging/common/version.sh); scripts/test-release-packaging.py
# checks the two agree. A branch build passes --define "upstream_version V"
# for the archive it actually has, as the Groundhog release does.
#
# nsync: Fedora has no nsync package (nostrc-dd5y); the copy in third_party/
# is linked statically and declared as bundled. `--with system_nsync` uses a
# packaged one instead.
#
# Named fipsd, not fips: Fedora's `fips` is an unrelated FITS image viewer
# that owns /usr/bin/fips. The daemon's binary is fipsd, so nothing collides.

%global upstream_version 0.1.1
%bcond_with system_nsync
%global fips_group fips
# libnostr-json under %%{_libdir}/fipsd is private to the daemon (an RPATH
# points there): neither provided to nor required from other packages.
%global __provides_exclude_from ^%{_libdir}/fipsd/.*$
%global __requires_exclude ^libnostr-json\\.so\\.2.*$

Name:           fipsd
Version:        %{upstream_version}
Release:        1%{?dist}
Summary:        FIPS mesh network daemon (C port)
License:        MIT
URL:            https://github.com/chebizarro/nostrc
Source0:        %{url}/releases/download/fipsd-v%{upstream_version}/fipsd-%{upstream_version}.tar.gz

BuildRequires:  cmake >= 3.22
BuildRequires:  ninja-build
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig
BuildRequires:  pkgconfig(glib-2.0) >= 2.50
BuildRequires:  pkgconfig(gobject-2.0) >= 2.50
BuildRequires:  pkgconfig(libsodium)
BuildRequires:  pkgconfig(yaml-0.1)
BuildRequires:  pkgconfig(jansson)
BuildRequires:  pkgconfig(libsecp256k1)
BuildRequires:  pkgconfig(openssl) >= 3.0
BuildRequires:  pkgconfig(libwebsockets)
BuildRequires:  pkgconfig(libcurl)
BuildRequires:  pkgconfig(libnftables)
# Shipped by systemd, needed by the scriptlets below.
BuildRequires:  systemd-rpm-macros
%if %{with system_nsync}
BuildRequires:  nsync-devel
%else
Provides:       bundled(nsync) = 1.29.2
%endif

Requires:       systemd
# groupadd, used by %%pre on hosts where systemd-sysusers has not run.
Requires(pre):  shadow-utils
# fips-firewall.service runs nft(8). Not required: the unit is opt-in and the
# daemon does not need it.
Recommends:     nftables

%description
FIPS is a distributed, decentralized network routing protocol for mesh nodes
connecting over arbitrary transports (UDP, TCP, and Nostr-mediated
rendezvous). fipsd is the nostrc C port of the daemon: encrypted
peer-to-peer links with automatic key management, a TUN-based IPv6 mesh
interface, a .fips resolver and a Unix control socket.

This package ships the daemon, its systemd units (fipsd, fips-dns and the
opt-in fips-firewall nftables baseline), the fips system group that gates
the control socket, and an example configuration seeded to
/etc/fips/fips.yaml on first install.

%package -n fips-gateway
Summary:        FIPS outbound LAN gateway
Requires:       fipsd%{?_isa} = %{version}-%{release}
Requires:       nftables
Requires:       iproute

%description -n fips-gateway
Linux IPv6 LAN-to-mesh gateway using libnftables NAT and proxy NDP.
Configure gateway in /etc/fips/fips.yaml before enabling the opt-in service.

%package -n libfipsnative
Summary:        Client library for the fipsd native datagram API

%description -n libfipsnative
libfipsnative is a plain-POSIX C client for the fipsd native datagram API:
programs link it to open flows over the mesh through the daemon's
SOCK_SEQPACKET API socket, with no dependency on the daemon's runtime or
protocol core.

%package -n libfipsnative-devel
Summary:        Development files for libfipsnative
Requires:       libfipsnative%{?_isa} = %{version}-%{release}

%description -n libfipsnative-devel
The header, static library and pkg-config file for libfipsnative, the C
client for the fipsd native datagram API.

%prep
%autosetup -n fipsd-%{upstream_version}

%build
# The daemon alone (libnostr with its GLib layer, no GObject/GTK), no tests
# (fipsd/packaging/common/cmake-args.sh is the one list every package reads).
%cmake -G Ninja $(sh fipsd/packaging/common/cmake-args.sh) \
    -DNOSTR_USE_SYSTEM_NSYNC=%{?with_system_nsync:ON}%{!?with_system_nsync:OFF} \
    -DGO_WARNINGS_AS_ERRORS=OFF \
    %{?fipsd_git_rev:-DFIPSD_GIT_REV=%{fipsd_git_rev}}
%cmake_build --target fipsd/all

%install
# Only fipsd's directory: the rest of the monorepo is not part of these
# packages. The payload (binaries, units, tmpfiles/sysusers fragments, the
# example config and the conffiles) is declared in fipsd/CMakeLists.txt.
DESTDIR=%{buildroot} cmake --install %{__cmake_builddir}/fipsd
# CMake fills in the unit templates; an unconfigured @VAR@ must never ship.
! grep -rq '@[A-Z_]*@' %{buildroot}%{_unitdir}
install -D -m 0644 LICENSE %{buildroot}%{_licensedir}/fipsd/LICENSE

%pre
# Control-socket access is by membership of the fips group, so it exists
# before the daemon creates the socket. The sysusers.d fragment creates it
# too where systemd-sysusers runs; this covers the rest.
getent group %{fips_group} >/dev/null || groupadd --system %{fips_group}

%post
# Seed /etc/fips/fips.yaml from the shipped example only if it does not
# already exist. The live config is deliberately not a packaged config file:
# copy-if-absent yields to any operator- or configuration-management-rendered
# file, never clobbers it, and never leaves a .rpmnew beside it either.
if [ ! -e %{_sysconfdir}/fips/fips.yaml ]; then
    install -m 0600 -o root -g root \
        %{_datadir}/fips/fips.yaml.example \
        %{_sysconfdir}/fips/fips.yaml
fi
%tmpfiles_create fipsd.conf
# Enabled on first install; fips-firewall.service stays opt-in. Enabled, not
# started: starting fips-dns.service would rewrite the host resolver inside
# the install transaction. An upgrade restarts what was running, below.
%systemd_post fipsd.service fips-dns.service
if [ $1 -eq 1 ] && [ -d /run/systemd/system ]; then
    systemctl enable fipsd.service fips-dns.service >/dev/null 2>&1 || :
fi
# On upgrade, reapply the firewall ruleset in place where the operator has it
# running: "try" leaves an inactive unit alone, and it must be a reload, not
# a restart, since the unit's ExecStop deletes the table.
if [ $1 -ge 2 ] && [ -d /run/systemd/system ]; then
    systemctl daemon-reload >/dev/null 2>&1 || :
    if ! systemctl try-reload-or-restart fips-firewall.service >/dev/null 2>&1; then
        echo "fipsd: reloading fips-firewall.service failed; the ruleset loaded before the upgrade stays in force" >&2
    fi
fi

%preun
# Stops and disables only on the last erase, not on an upgrade.
%systemd_preun fipsd.service fips-dns.service fips-firewall.service

%postun
# Restarts what was running, on upgrade only. Queued rather than waited on:
# this runs inside the rpm transaction holding dnf's lock, and fips-dns is a
# oneshot ordered after the daemon.
if [ $1 -ge 1 ] && [ -d /run/systemd/system ]; then
    systemctl --no-block try-restart fipsd.service fips-dns.service >/dev/null 2>&1 || :
fi
if [ $1 -eq 0 ]; then
    # The runtime directory is not packaged, so nothing else removes it.
    rm -rf /run/fips
    # DNS routing fips-dns-setup may have written outside the package, one
    # file per backend, in case fips-dns-teardown did not run. rpm has no
    # purge, so this erase branch is the only cleanup that will ever run.
    restart_resolved=0
    for f in %{_sysconfdir}/systemd/dns-delegate.d/fips.dns-delegate \
             %{_sysconfdir}/systemd/resolved.conf.d/fips.conf; do
        if [ -f "$f" ]; then rm -f "$f"; restart_resolved=1; fi
    done
    if [ "$restart_resolved" = 1 ] && [ -d /run/systemd/system ] \
        && systemctl is-active --quiet systemd-resolved.service; then
        systemctl restart systemd-resolved \
            || echo "fipsd: warning: could not restart systemd-resolved; restart it to drop the .fips route"
    fi
    if [ -f %{_sysconfdir}/dnsmasq.d/fips.conf ]; then
        rm -f %{_sysconfdir}/dnsmasq.d/fips.conf
        [ -d /run/systemd/system ] && systemctl try-restart dnsmasq >/dev/null 2>&1 || :
    fi
    if [ -f %{_sysconfdir}/NetworkManager/dnsmasq.d/fips.conf ]; then
        rm -f %{_sysconfdir}/NetworkManager/dnsmasq.d/fips.conf
        command -v nmcli >/dev/null 2>&1 && nmcli general reload >/dev/null 2>&1 || :
    fi
fi
# Not here, on purpose: /etc/fips and the fips group survive an erase. rpm
# has no purge, and the equivalent of the Debian purge would take the node's
# identity keys with it during a distribution upgrade that erases and
# reinstalls. Remove /etc/fips yourself if you mean it.

%post -n fips-gateway
# Opt-in: reload unit definitions, but do not enable or start on install.
if [ -d /run/systemd/system ]; then
    systemctl daemon-reload >/dev/null 2>&1 || :
fi

%preun -n fips-gateway
%systemd_preun fips-gateway.service

%postun -n fips-gateway
%systemd_postun_with_restart fips-gateway.service
if [ $1 -eq 0 ]; then
    rm -rf /run/fips-gateway
fi

%files -n fips-gateway
%license %{_licensedir}/fipsd/LICENSE
%{_bindir}/fips-gateway
%{_unitdir}/fips-gateway.service

%files
%license %{_licensedir}/fipsd/LICENSE
%{_bindir}/fipsd
%dir %{_libdir}/fipsd
%{_libdir}/fipsd/libnostr-json.so.2*
%{_unitdir}/fipsd.service
%{_unitdir}/fips-dns.service
%{_unitdir}/fips-firewall.service
%{_tmpfilesdir}/fipsd.conf
%{_sysusersdir}/fipsd.conf
%dir %{_datadir}/fips
%{_datadir}/fips/fips.yaml.example
%dir %{_sysconfdir}/fips
%dir %{_sysconfdir}/fips/fips.d
%config(noreplace) %{_sysconfdir}/fips/hosts
%config(noreplace) %{_sysconfdir}/fips/fips.nft
# fipsctl and the fips-dns helpers are packaged when the tree has them.
%{?_fipsd_has_fipsctl:%{_bindir}/fipsctl}
%{?_fipsd_has_dns_helpers:%dir %{_prefix}/lib/fips}
%{?_fipsd_has_dns_helpers:%{_prefix}/lib/fips/fips-dns-setup}
%{?_fipsd_has_dns_helpers:%{_prefix}/lib/fips/fips-dns-teardown}

%files -n libfipsnative
%license %{_licensedir}/fipsd/LICENSE
%{_libdir}/libfipsnative.so.0*

%files -n libfipsnative-devel
%{_includedir}/fips-native-client.h
%{_includedir}/fipsnative-version.h
%{_libdir}/libfipsnative.so
%{_libdir}/libfipsnative.a
%{_libdir}/pkgconfig/libfipsnative.pc

%changelog
* Fri Nov 20 2026 Biz <chebizarro@protonmail.com> - 0.1.1-1
- fips.yaml.example: the commented peer example uses the schema the daemon
  accepts (addresses: [{transport, addr}]) instead of peer-level
  address/transport, which the daemon rejected.

* Mon Nov 16 2026 Biz <chebizarro@protonmail.com> - 0.1.0-1
- Initial packaging of the nostrc C port, translated from the Debian recipe.
