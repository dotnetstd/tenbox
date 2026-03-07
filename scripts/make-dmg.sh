#!/bin/bash
# Package TenBox.app into a DMG disk image for distribution.
#
# Usage:
#   ./make-dmg.sh [path/to/TenBox.app] [output.dmg]
#
# Prerequisites:
#   - TenBox.app must be a valid macOS application bundle
#   - create-dmg (brew install create-dmg) is recommended but not required
#
# The script creates a DMG with:
#   - TenBox.app
#   - A symlink to /Applications for drag-and-drop install
#   - tenbox-vm-runtime bundled inside the .app

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../build"

APP_PATH="${1:-$BUILD_DIR/TenBox.app}"
OUTPUT="${2:-$BUILD_DIR/TenBox.dmg}"
VOLUME_NAME="TenBox"
TEMP_DMG="$BUILD_DIR/TenBox-temp.dmg"

if [ ! -d "$APP_PATH" ]; then
    echo "Error: Application bundle not found: $APP_PATH"
    echo ""
    echo "Build TenBox.app first:"
    echo "  1. Open src/manager-macos in Xcode"
    echo "  2. Build for Release (Product -> Archive)"
    echo "  3. Export the .app bundle"
    echo ""
    echo "Or use CMake to build the runtime:"
    echo "  mkdir -p build && cd build"
    echo "  cmake .. -DCMAKE_BUILD_TYPE=Release"
    echo "  make -j\$(sysctl -n hw.ncpu)"
    exit 1
fi

# Ensure the runtime binary is inside the app bundle
RUNTIME_IN_APP="$APP_PATH/Contents/MacOS/tenbox-vm-runtime"
RUNTIME_BUILD="$BUILD_DIR/tenbox-vm-runtime"
if [ ! -f "$RUNTIME_IN_APP" ] && [ -f "$RUNTIME_BUILD" ]; then
    echo "Copying runtime into app bundle..."
    cp "$RUNTIME_BUILD" "$RUNTIME_IN_APP"
fi

# Sign the app if a signing identity is available
if security find-identity -v -p codesigning 2>/dev/null | grep -q "Developer ID"; then
    IDENTITY=$(security find-identity -v -p codesigning | grep "Developer ID" | head -1 | awk -F'"' '{print $2}')
    echo "Signing with: $IDENTITY"
    codesign --deep --force --options runtime \
        --entitlements "$SCRIPT_DIR/../src/manager-macos/Resources/TenBox.entitlements" \
        --sign "$IDENTITY" "$APP_PATH"
else
    echo "No Developer ID found, signing with ad-hoc identity..."
    codesign --deep --force --options runtime \
        --entitlements "$SCRIPT_DIR/../src/manager-macos/Resources/TenBox.entitlements" \
        --sign - "$APP_PATH"
fi

# Remove old DMG if exists
rm -f "$OUTPUT" "$TEMP_DMG"

# Try create-dmg if available (pretty DMG with background)
if command -v create-dmg &>/dev/null; then
    echo "Creating DMG with create-dmg..."
    create-dmg \
        --volname "$VOLUME_NAME" \
        --window-pos 200 120 \
        --window-size 600 400 \
        --icon-size 100 \
        --icon "TenBox.app" 150 200 \
        --app-drop-link 450 200 \
        --no-internet-enable \
        "$OUTPUT" \
        "$APP_PATH"
else
    echo "Creating DMG with hdiutil..."
    SIZE_KB=$(du -sk "$APP_PATH" | cut -f1)
    SIZE_KB=$((SIZE_KB + 10240))  # Add 10MB headroom

    hdiutil create -size "${SIZE_KB}k" -fs HFS+ -volname "$VOLUME_NAME" \
        -type SPARSE "$TEMP_DMG"

    MOUNT_POINT=$(hdiutil attach "${TEMP_DMG}.sparseimage" -readwrite -noverify | \
        grep "/Volumes/$VOLUME_NAME" | awk '{print $NF}')

    cp -R "$APP_PATH" "$MOUNT_POINT/"
    ln -s /Applications "$MOUNT_POINT/Applications"

    # Eject
    hdiutil detach "$MOUNT_POINT"

    # Convert sparse to compressed DMG
    hdiutil convert "${TEMP_DMG}.sparseimage" -format UDZO \
        -imagekey zlib-level=9 -o "$OUTPUT"

    rm -f "${TEMP_DMG}.sparseimage"
fi

echo ""
echo "============================================"
echo "DMG created: $OUTPUT ($(du -h "$OUTPUT" | cut -f1))"
echo "============================================"
