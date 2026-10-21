Name:           rocket-switcher
# deb_upstream / deb_revision are passed by the RPM build from the first
# stanza of packaging/deb/changelog, so the .rpm revision matches the .deb.
Version:        %{deb_upstream}
Release:        %{deb_revision}%{?dist}
Summary:        Ракета — automatic ru↔en keyboard layout corrector for Wayland

License:        GPL-2.0+
URL:            https://github.com/vsdesk/rocket-switcher
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.20
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig
BuildRequires:  qt6-qtbase-devel
BuildRequires:  gtest-devel
BuildRequires:  extra-cmake-modules
BuildRequires:  libevdev-devel
BuildRequires:  libxkbcommon-devel

# Runtime dependencies — only what the binary actually links against
Requires:       qt6-qtbase
Requires:       wl-clipboard
Requires:       wtype
Obsoletes:      punto-switcher
Provides:       punto-switcher = %{version}-%{release}

%description
Ракета provides Punto Switcher-like functionality for Wayland:
automatic detection and correction of text typed in the wrong keyboard
layout (Russian/English), manual swap of the last typed word, and swap
of selected text.

Features:
 - Auto-switch: detects wrong layout at word boundaries (bigram scoring)
 - Swap last word hotkey (default: Alt+')
 - Swap selection hotkey (default: Alt+Shift+')
 - Toggle auto-switch hotkey (default: Alt+Shift+A)
 - Qt6 configuration GUI

%prep
%autosetup

%build
%cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=ON \
    -DCMAKE_INSTALL_PREFIX=%{_prefix} \
    -DCMAKE_INSTALL_LIBDIR=%{_libdir}
%cmake_build

%install
%cmake_install

%check
# Run unit tests via ctest to avoid relying on a generator-specific "test" target.
ctest --test-dir redhat-linux-build --output-on-failure

%files
%license LICENSE
%doc README.md
%{_bindir}/rocket-switcher-daemon
%{_bindir}/rocket-switcher-stop
%{_bindir}/rocket-switcher-config
%{_datadir}/applications/rocket-switcher-config.desktop
%{_datadir}/rocket-switcher

%changelog
* Mon Oct 19 2026 Raketa Maintainer <maintainer@example.com> - 1.0.0-138
- Name the package rocket-switcher and take the revision from the Debian changelog
* Mon Oct 19 2026 Raketa Maintainer <maintainer@example.com> - 1.0.0-1
- Package the dictionaries installed under /usr/share/rocket-switcher/dict
* Wed Mar 25 2026 PuntoSwitcher Maintainer <maintainer@example.com> - 1.0.0-1
- Initial release
