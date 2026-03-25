#!/usr/bin/env bash
# Same flow as the earlier PUNTO session: snapshot sources into _dist/punto-switcher-<upstream>/
# via tar excludes (no nested _dist/_build/.git), add debian/ from packaging/deb, dpkg-buildpackage.
# Binary .deb lands in _dist/; we also copy to dist/ and to repo root as punto-switcher_<ver>_latest.deb.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if ! command -v dpkg-buildpackage >/dev/null; then
  echo "Install: sudo apt-get install dpkg-dev debhelper fakeroot" >&2
  exit 1
fi

CHANGELOG="$ROOT/packaging/deb/changelog"
if [[ ! -f "$CHANGELOG" ]]; then
  echo "Missing $CHANGELOG" >&2
  exit 1
fi

FULL_VER=$(grep -m1 '^punto-switcher' "$CHANGELOG" | sed -n 's/.*(\([^)]*\)).*/\1/p')
# Upstream directory name: strip last -<debian_revision> (e.g. 1.0.0-20 -> 1.0.0)
UPSTREAM_VER=$(echo "$FULL_VER" | sed 's/-[0-9][0-9]*$//')
SRCDIR="$ROOT/_dist/punto-switcher-${UPSTREAM_VER}"

echo "==> Version from changelog: $FULL_VER (upstream dir: punto-switcher-${UPSTREAM_VER})"

mkdir -p "$ROOT/_dist"
rm -rf "$SRCDIR"
mkdir -p "$SRCDIR"

echo "==> Extracting source snapshot into $SRCDIR"
tar -C "$ROOT" \
  --exclude='./_dist' \
  --exclude='./_build' \
  --exclude='./build' \
  --exclude='./debian' \
  --exclude='./dist' \
  --exclude='./.git' \
  --exclude='./.cursor' \
  --exclude='./obj-*' \
  --exclude='./*.deb' \
  -cf - . | (cd "$SRCDIR" && tar -xf -)

cp -r "$ROOT/packaging/deb" "$SRCDIR/debian"

cd "$SRCDIR"

if ! dpkg-checkbuilddeps 2>/dev/null; then
  echo "Install build dependencies:" >&2
  echo "  sudo apt-get install debhelper cmake pkg-config dh-cmake qt6-base-dev libevdev-dev libxkbcommon-dev" >&2
  exit 1
fi

echo "==> dpkg-buildpackage in $SRCDIR"
fakeroot dpkg-buildpackage -us -uc -b -j"$(nproc)"

ARCH="$(dpkg-architecture -qDEB_HOST_ARCH 2>/dev/null || echo amd64)"
MAIN_DEB="$ROOT/_dist/punto-switcher_${FULL_VER}_${ARCH}.deb"

mkdir -p "$ROOT/dist"
if [[ ! -f "$MAIN_DEB" ]]; then
  echo "Expected package not found: $MAIN_DEB" >&2
  exit 1
fi

cp -v "$MAIN_DEB" "$ROOT/dist/"
LATEST="$ROOT/punto-switcher_${FULL_VER}_latest.deb"
rm -f "$ROOT"/punto-switcher_*_latest.deb
cp -v "$MAIN_DEB" "$LATEST"

echo "==> Done:"
ls -la "$MAIN_DEB" "$ROOT/dist/punto-switcher_${FULL_VER}_${ARCH}.deb" "$LATEST"
echo ""
echo "Install (prefer dpkg for a .deb under \$HOME — avoids _apt permission warnings):"
echo "  sudo dpkg -i $LATEST && sudo apt-get -f install -y"
echo "Or copy to /tmp then apt:"
echo "  cp $LATEST /tmp/punto-switcher.deb && sudo apt install -y /tmp/punto-switcher.deb"
