#!/bin/bash
# make-appimage.sh — build rufus-linux.AppImage
#
# Prerequisites (downloaded automatically on first run):
#   linuxdeploy-x86_64.AppImage
#   linuxdeploy-plugin-gtk.sh
#   appimagetool-x86_64.AppImage
#
# The resulting rufus-linux-x86_64.AppImage is a single file that bundles
# GTK4 and all other shared libraries. Run it on any x86_64 Linux >= glibc 2.17.
#
# NOTE: writing to USB devices still needs root. Launch via:
#   sudo ./rufus-linux-x86_64.AppImage
#   pkexec ./rufus-linux-x86_64.AppImage   (if polkit action is installed)

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

ARCH=x86_64
APPDIR="$SCRIPT_DIR/AppDir"
TOOLS_DIR="$SCRIPT_DIR/.appimage-tools"
mkdir -p "$TOOLS_DIR"

# ── Download helpers ──────────────────────────────────────────────────────────
download() {
    local url="$1" dst="$2"
    if [[ ! -f "$dst" ]]; then
        echo "Downloading $(basename "$dst")…"
        curl -fsSL "$url" -o "$dst"
        chmod +x "$dst"
    fi
}

LINUXDEPLOY="$TOOLS_DIR/linuxdeploy-$ARCH.AppImage"
LINUXDEPLOY_GTK="$TOOLS_DIR/linuxdeploy-plugin-gtk.sh"
APPIMAGETOOL="$TOOLS_DIR/appimagetool-$ARCH.AppImage"

download "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$ARCH.AppImage" "$LINUXDEPLOY"
download "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh" "$LINUXDEPLOY_GTK"
download "https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-$ARCH.AppImage" "$APPIMAGETOOL"
export PATH="$TOOLS_DIR:$PATH"

# ── Build the binary ──────────────────────────────────────────────────────────
echo "Building rufus-linux…"
meson setup build --wipe -Dprefix=/usr --buildtype=release 2>&1 | tail -5
ninja -C build

# ── Populate AppDir ───────────────────────────────────────────────────────────
rm -rf "$APPDIR"
DESTDIR="$APPDIR" ninja -C build install

# AppImage requires a .desktop file and an icon at AppDir root.
mkdir -p "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps"

cat > "$APPDIR/usr/share/applications/rufus-linux.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Rufus Linux
GenericName=Bootable USB creator
Comment=Format and create bootable USB drives
Exec=rufus-linux
Icon=rufus-linux
Categories=System;Utility;
Terminal=false
EOF

# Use a simple placeholder icon if none exists yet.
ICON_SRC="$APPDIR/usr/share/icons/hicolor/256x256/apps/rufus-linux.png"
if [[ ! -f "$ICON_SRC" ]]; then
    # Generate a minimal 1×1 PNG with ImageMagick if available, else skip.
    if command -v convert &>/dev/null; then
        convert -size 256x256 xc:"#c0392b" "$ICON_SRC" 2>/dev/null || true
    fi
fi

# ── Bundle with linuxdeploy + GTK plugin ─────────────────────────────────────
echo "Bundling libraries…"
export DEPLOY_GTK_VERSION=4
export GTK_THEME=Adwaita   # ship a known-good theme
LINUXDEPLOY_OUTPUT_VERSION="$(date +%Y%m%d)" \
"$LINUXDEPLOY" \
    --appdir "$APPDIR" \
    --plugin gtk \
    --desktop-file "$APPDIR/usr/share/applications/rufus-linux.desktop" \
    --output appimage 2>&1

# linuxdeploy writes to the current directory.
RESULT=$(ls rufus-linux*AppImage 2>/dev/null | sort | tail -1)
if [[ -z "$RESULT" ]]; then
    echo "ERROR: AppImage not produced. Check output above."
    exit 1
fi
echo ""
echo "✓ Built: $SCRIPT_DIR/$RESULT"
echo "  Run:   sudo ./$RESULT"
