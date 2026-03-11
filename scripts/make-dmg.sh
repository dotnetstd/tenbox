#!/bin/bash
# Package TenBox.app into a DMG disk image for distribution.
#
# Usage:
#   ./make-dmg.sh [path/to/TenBox.app] [output.dmg]
#
# Prerequisites:
#   - TenBox.app must be a valid macOS application bundle
#
# The script creates a DMG with:
#   - TenBox.app
#   - A symlink to /Applications for drag-and-drop install
#   - tenbox-vm-runtime bundled inside the .app

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/.."
BUILD_DIR="$ROOT_DIR/build"

VERSION=$(tr -d '[:space:]' < "$ROOT_DIR/VERSION")
if [ -z "$VERSION" ]; then
    echo "Error: Could not read version from $ROOT_DIR/VERSION"
    exit 1
fi

ARCH=$(uname -m)  # arm64 or x86_64

APP_PATH="${1:-$BUILD_DIR/TenBox.app}"
OUTPUT="${2:-$BUILD_DIR/TenBox_${VERSION}_${ARCH}.dmg}"
VOLUME_NAME="TenBox"

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
ENTITLEMENTS="$SCRIPT_DIR/../src/manager-macos/Resources/TenBox.entitlements"
if security find-identity -v -p codesigning 2>/dev/null | grep -q "Developer ID"; then
    IDENTITY=$(security find-identity -v -p codesigning | grep "Developer ID" | head -1 | awk -F'"' '{print $2}')
    echo "Signing with: $IDENTITY"
    codesign --deep --force --options runtime \
        --entitlements "$ENTITLEMENTS" \
        --sign "$IDENTITY" "$APP_PATH"
else
    echo "No Developer ID found, signing with ad-hoc identity..."
    codesign --deep --force --options runtime \
        --entitlements "$ENTITLEMENTS" \
        --sign - "$APP_PATH"
fi

# ── Create DMG ───────────────────────────────────────────────────────────────
# Uses APFS (required for macOS 26 Tahoe where HFS+ has mounting bugs) and
# a temporary volume name to avoid Gatekeeper blocking ditto when the .app
# name matches the volume name. Produces a styled DMG with 128px icons,
# drag-and-drop arrow background, and correct Finder view settings.

rm -f "$OUTPUT"

echo "Building styled DMG..."
python3 "$SCRIPT_DIR/make-dmg-styled.py" "$APP_PATH" "$VOLUME_NAME" "$OUTPUT"

echo "  -> DMG created: $OUTPUT"

# ── Notarize the DMG ────────────────────────────────────────────────────────
if xcrun notarytool history --keychain-profile "AC_PASSWORD" >/dev/null 2>&1; then
    echo ""
    echo "Submitting DMG for Apple notarization..."
    echo "(This typically takes 2-10 minutes)"

    xcrun notarytool submit "$OUTPUT" \
        --keychain-profile "AC_PASSWORD" \
        --wait
    NOTARY_EXIT=$?

    if [ $NOTARY_EXIT -eq 0 ]; then
        echo ""
        echo "Stapling notarization ticket to DMG..."
        xcrun stapler staple "$OUTPUT"
        echo "Notarization complete!"
    else
        echo ""
        echo "WARNING: Notarization failed (exit code $NOTARY_EXIT)."
        echo "  Check details: xcrun notarytool log <submission-id> --keychain-profile AC_PASSWORD"
        exit 1
    fi
else
    echo ""
    echo "Skipping notarization (no AC_PASSWORD keychain profile found)."
    echo "To enable, run:"
    echo "  xcrun notarytool store-credentials AC_PASSWORD \\"
    echo "      --apple-id YOUR_APPLE_ID --team-id YOUR_TEAM_ID --password APP_SPECIFIC_PASSWORD"
fi

echo ""
echo "============================================"
echo "DMG created: $OUTPUT ($(du -h "$OUTPUT" | cut -f1))"
echo "============================================"
