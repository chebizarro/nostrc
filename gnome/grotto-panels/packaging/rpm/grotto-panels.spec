# SPDX-License-Identifier: MIT
Name:           grotto-panels
Version:        0.1.0
Release:        1%{?dist}
Summary:        Public-metadata GTK management panel runtime
License:        MIT
URL:            https://github.com/chebizarro/nostrc
Source0:        grotto-panels-%{version}.tar.gz
BuildRequires:  cmake >= 3.22
BuildRequires:  ninja-build
BuildRequires:  gcc
BuildRequires:  pkgconfig(gtk4) >= 4.14
BuildRequires:  pkgconfig(libadwaita-1) >= 1.5
Requires:       gtk4 >= 4.14
Requires:       libadwaita >= 1.5

%description
Injected public-metadata management panel infrastructure. No signer policy or
key custody is implemented in this shared library.

%package devel
Summary:        Headers and build metadata for grotto-panels
Requires:       %{name}%{?_isa} = %{version}-%{release}
Requires:       pkgconfig(gtk4) >= 4.14
Requires:       pkgconfig(libadwaita-1) >= 1.5

%description devel
Public headers, pkg-config and CMake package for grotto-panels-0 consumers.

%prep
%autosetup

%build
cmake -S gnome/grotto-panels -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    -DCMAKE_INSTALL_PREFIX=%{_prefix} -DCMAKE_INSTALL_LIBDIR=%{_libdir}
cmake --build build --parallel 2

%install
DESTDIR=%{buildroot} cmake --install build

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files
%license gnome/grotto-panels/LICENSE
%{_libdir}/libgrotto-panels-0.so.0*

%files devel
%{_includedir}/grotto-panels-0/
%{_libdir}/libgrotto-panels-0.so
%{_libdir}/pkgconfig/grotto-panels-0.pc
%{_libdir}/cmake/GrottoPanels/
