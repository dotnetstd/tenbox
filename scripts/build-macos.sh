#!/bin/bash
# Build TenBox for macOS (Apple Silicon).
#
# This script builds:
#   1. tenbox-vm-runtime (C++ via CMake)
#   2. TenBox.app (Swift/Obj-C++ manager — currently requires Xcode)
#
# Usage:
#   ./build-macos.sh [--release|--debug]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/.."
BUILD_TYPE="${1:---release}"
CPU_COUNT=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)

case "$BUILD_TYPE" in
    --release) CMAKE_BUILD_TYPE="Release" ;;
    --debug)   CMAKE_BUILD_TYPE="Debug" ;;
    *)
        echo "Usage: $0 [--release|--debug]"
        exit 1
        ;;
esac

VERSION=$(tr -d '[:space:]' < "$ROOT_DIR/VERSION")
if [ -z "$VERSION" ]; then
    echo "Error: Could not read version from $ROOT_DIR/VERSION"
    exit 1
fi

echo "===================================="
echo " TenBox macOS Build v$VERSION ($CMAKE_BUILD_TYPE)"
echo "===================================="
echo ""

# Stamp the version into Info.plist before building
PLIST="$ROOT_DIR/src/manager-macos/Resources/Info.plist"
/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VERSION" "$PLIST"
echo "Version $VERSION written to Info.plist"
echo ""

# Step 1: Build the C++ runtime via CMake
echo "[1/2] Building tenbox-vm-runtime..."
BUILD_DIR="$ROOT_DIR/build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake "$ROOT_DIR" \
    -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
    -DCMAKE_OSX_ARCHITECTURES=arm64

cmake --build . --target tenbox-vm-runtime -j"$CPU_COUNT"

if [ ! -f "$BUILD_DIR/tenbox-vm-runtime" ]; then
    echo "Error: tenbox-vm-runtime binary not found after build."
    exit 1
fi
echo "  -> $BUILD_DIR/tenbox-vm-runtime"

# Step 2: Build the macOS manager
echo ""
echo "[2/2] Building TenBox Manager (SwiftUI)..."
echo ""
echo "The macOS Manager GUI requires Xcode to build."
echo "Please open the following in Xcode:"
echo ""
echo "  $ROOT_DIR/src/manager-macos/"
echo ""
echo "Or build from command line if an Xcode project exists:"
echo "  xcodebuild -project TenBox.xcodeproj -scheme TenBox -configuration $CMAKE_BUILD_TYPE"
echo ""
echo "After building the Manager, copy tenbox-vm-runtime into the app bundle:"
echo "  cp $BUILD_DIR/tenbox-vm-runtime TenBox.app/Contents/MacOS/"
echo ""
echo "Then create the DMG:"
echo "  $SCRIPT_DIR/make-dmg.sh TenBox.app"
echo ""
echo "===================================="
echo " Runtime build complete."
echo "===================================="
