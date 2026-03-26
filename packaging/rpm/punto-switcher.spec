Name:           punto-switcher
Version:        1.0.0
Release:        1%{?dist}
Summary:        Punto-style automatic ru<->en keyboard layout corrector for Wayland/Fcitx5

License:        GPL-2.0+
URL:            https://github.com/your-org/punto-switcher
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.20
BuildRequires:  gcc-c++
BuildRequires:  pkgconfig
BuildRequires:  fcitx5-devel
BuildRequires:  qt6-qtbase-devel
BuildRequires:  gtest-devel
BuildRequires:  extra-cmake-modules

# Runtime dependencies — only what the binary actually links against
Requires:       fcitx5
Requires:       fcitx5-libs
Requires:       qt6-qtbase
Recommends:     kcm-fcitx5

%description
PuntoSwitcher is a Fcitx5 module that provides Punto Switcher-like
functionality for Wayland: automatic detection and correction of text
typed in the wrong keyboard layout (Russian/English), manual swap of
the last typed word, and swap of selected text.

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
    -DBUILD_TESTING=OFF \
    -DCMAKE_INSTALL_PREFIX=%{_prefix} \
    -DCMAKE_INSTALL_LIBDIR=%{_libdir}
%cmake_build

%install
%cmake_install

%check
# Run unit tests during package build (optional; requires gtest)
# %cmake_build --target test

%files
%license LICENSE
%doc README.md
%{_libdir}/fcitx5/libpunto-switcher-engine.so
%{_datadir}/fcitx5/addon/punto-switcher.conf
%{_bindir}/punto-switcher-config
%{_datadir}/applications/punto-switcher-config.desktop

%changelog
* Wed Mar 25 2026 PuntoSwitcher Maintainer <maintainer@example.com> - 1.0.0-1
- Initial release
