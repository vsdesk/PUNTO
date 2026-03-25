#!/usr/bin/env bash
# build-and-install.sh — сборка и установка PuntoSwitcher на Ubuntu/Debian/Fedora
# Запускать из директории репозитория: bash build-and-install.sh
# Требует: sudo

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/_build"
INSTALL_PREFIX="/usr"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Detect distro
# ---------------------------------------------------------------------------
if   command -v apt-get  &>/dev/null; then PKG_MGR=apt
elif command -v dnf      &>/dev/null; then PKG_MGR=dnf
else error "Unsupported distro (need apt or dnf)"; fi

info "Detected package manager: $PKG_MGR"
info "Build directory: $BUILD_DIR"

# ---------------------------------------------------------------------------
# Install build dependencies
# ---------------------------------------------------------------------------
install_deps_apt() {
    info "Installing build dependencies (apt)..."
    sudo apt-get update -qq
    sudo apt-get install -y --no-install-recommends \
        cmake build-essential pkg-config \
        libfcitx5core-dev libfcitx5utils-dev libfcitx5config-dev \
        fcitx5 fcitx5-modules \
        qt6-base-dev \
        libgtest-dev googletest \
        extra-cmake-modules \
        debhelper devscripts dpkg-dev fakeroot
}

install_deps_dnf() {
    info "Installing build dependencies (dnf)..."
    sudo dnf install -y \
        cmake gcc-c++ pkgconfig \
        fcitx5-devel \
        qt6-qtbase-devel \
        gtest-devel \
        extra-cmake-modules \
        rpm-build rpmdevtools
}

if [ "$PKG_MGR" = "apt" ]; then
    install_deps_apt
else
    install_deps_dnf
fi

# ---------------------------------------------------------------------------
# Build with CMake
# ---------------------------------------------------------------------------
info "Configuring CMake..."
cmake -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
    -DBUILD_TESTING=ON \
    -S "$SCRIPT_DIR"

info "Building..."
cmake --build "$BUILD_DIR" -j"$(nproc)"

# ---------------------------------------------------------------------------
# Run unit tests
# ---------------------------------------------------------------------------
info "Running unit tests..."
if ctest --test-dir "$BUILD_DIR" --output-on-failure -V; then
    info "All tests passed ✓"
else
    warn "Some tests failed — continuing with installation (check output above)"
fi

# ---------------------------------------------------------------------------
# Build .deb (apt systems)
# ---------------------------------------------------------------------------
if [ "$PKG_MGR" = "apt" ]; then
    info "Building .deb package..."
    DIST_DIR="$SCRIPT_DIR/_dist"
    mkdir -p "$DIST_DIR"

    # Create a source tree for dpkg-buildpackage
    BUILD_PKG_DIR="$DIST_DIR/punto-switcher-1.0.0"
    rm -rf "$BUILD_PKG_DIR"
    cp -r "$SCRIPT_DIR" "$BUILD_PKG_DIR"
    cp -r "$SCRIPT_DIR/packaging/deb" "$BUILD_PKG_DIR/debian"

    # orig tarball
    tar czf "$DIST_DIR/punto-switcher_1.0.0.orig.tar.gz" \
        -C "$DIST_DIR" punto-switcher-1.0.0/

    cd "$BUILD_PKG_DIR"
    dpkg-buildpackage -us -uc -b -j"$(nproc)" 2>&1 | grep -v "^dpkg-source\|^dpkg-genchanges"

    DEB_FILE=$(ls "$DIST_DIR"/*.deb 2>/dev/null | head -1)
    if [ -z "$DEB_FILE" ]; then
        error "No .deb file found after build"
    fi
    info "Package built: $DEB_FILE"

    # Install
    info "Installing .deb..."
    sudo dpkg -i "$DEB_FILE" || sudo apt-get install -f -y
    info "Installed successfully"

# ---------------------------------------------------------------------------
# Build .rpm (Fedora)
# ---------------------------------------------------------------------------
elif [ "$PKG_MGR" = "dnf" ]; then
    info "Building .rpm package..."
    rpmdev-setuptree
    TARBALL_NAME="punto-switcher-1.0.0.tar.gz"
    tar czf ~/rpmbuild/SOURCES/"$TARBALL_NAME" \
        --transform 's,^,punto-switcher-1.0.0/,' \
        -C "$SCRIPT_DIR" .
    cp "$SCRIPT_DIR/packaging/rpm/punto-switcher.spec" ~/rpmbuild/SPECS/
    rpmbuild -bb ~/rpmbuild/SPECS/punto-switcher.spec

    RPM_FILE=$(find ~/rpmbuild/RPMS -name "*.rpm" | head -1)
    info "Package built: $RPM_FILE"

    info "Installing .rpm..."
    sudo dnf install -y "$RPM_FILE"
    info "Installed successfully"
fi

# ---------------------------------------------------------------------------
# Activate the Fcitx5 module
# ---------------------------------------------------------------------------
info "Activating Fcitx5 module..."

if pgrep -x fcitx5 &>/dev/null; then
    fcitx5-remote -r
    info "Fcitx5 reloaded (hot-reload)"
else
    warn "Fcitx5 is not running. Start it with: fcitx5 -d"
    warn "Or set it as default IM via: im-config -n fcitx5"
fi

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
echo ""
echo -e "${GREEN}============================================${NC}"
echo -e "${GREEN}  PuntoSwitcher installed successfully!${NC}"
echo -e "${GREEN}============================================${NC}"
echo ""
echo "  GUI:     punto-switcher-config"
echo "  Config:  ~/.config/fcitx5/conf/punto-switcher.conf"
echo ""
echo "  Default hotkeys:"
echo "    Swap last word:    Alt + '"
echo "    Swap selection:    Alt + Shift + '"
echo "    Toggle auto-switch: Alt + Shift + A"
echo ""
echo "  If Fcitx5 was not running, start it and verify:"
echo "    fcitx5 -d && fcitx5-remote -l | grep punto"
echo ""
