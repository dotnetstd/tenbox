#!/bin/bash
# Build virtio_snd.ko for arm64 from Debian kernel source.
# Must run inside an arm64 Docker container (via build.sh or manually).
#
# Usage:
#   ./scripts/docker/build.sh arm64 virtio-snd
#   # or directly inside an arm64 container:
#   ./scripts/arm64/build-virtio-snd.sh [output_dir] [suite]
#
# Output: virtio_snd.ko placed into output_dir (default: scripts/arm64/extra-modules/)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUTDIR="${1:-$SCRIPT_DIR/extra-modules}"
SUITE="${2:-bookworm}"
WORKDIR=$(mktemp -d)
trap "rm -rf $WORKDIR" EXIT

MIRROR="https://deb.debian.org/debian"
DEB_ARCH="arm64"

mkdir -p "$OUTDIR"

echo "=== Building virtio_snd.ko for $DEB_ARCH ($SUITE) ==="

cd "$WORKDIR"

echo "[1/5] Resolving kernel version ..."
curl -fsSL "$MIRROR/dists/$SUITE/main/binary-$DEB_ARCH/Packages.gz" | gunzip > Packages

META_BLOCK=$(awk '/^Package: linux-image-arm64$/,/^$/' Packages)
KPKG=$(echo "$META_BLOCK" | sed -n 's/^Depends:.*\(linux-image-[0-9][^ ,]*\).*/\1/p')
KVER=$(echo "$KPKG" | sed 's/^linux-image-//')
if [ -z "$KPKG" ] || [ -z "$KVER" ]; then
    echo "Error: could not resolve kernel package from $SUITE/$DEB_ARCH." >&2
    exit 1
fi
echo "  Kernel package: $KPKG"
echo "  Kernel version: $KVER"

# Debian linux-source package uses major.minor (e.g. "6.1" from "6.1.0-42-arm64")
SRC_VER=$(echo "$KVER" | sed 's/^\([0-9]*\.[0-9]*\).*/\1/')
echo "  Source version:  linux-source-$SRC_VER"

echo "[2/5] Installing build dependencies ..."
apt-get update -qq
apt-get install -y --no-install-recommends \
    build-essential bc kmod cpio curl ca-certificates \
    linux-headers-${KVER} \
    linux-source-${SRC_VER}

HEADER_DIR="/usr/src/linux-headers-${KVER}"
if [ ! -d "$HEADER_DIR" ]; then
    HEADER_DIR="/lib/modules/${KVER}/build"
fi
if [ ! -d "$HEADER_DIR" ]; then
    echo "Error: kernel headers not found." >&2
    echo "  Searched: /usr/src/linux-headers-${KVER}"
    echo "  Searched: /lib/modules/${KVER}/build"
    exit 1
fi
echo "  Headers: $HEADER_DIR"

SRC_TARBALL=$(ls /usr/src/linux-source-${SRC_VER}.tar.* 2>/dev/null | head -1)
if [ -z "$SRC_TARBALL" ]; then
    echo "Error: kernel source tarball not found at /usr/src/linux-source-${SRC_VER}.tar.*" >&2
    ls /usr/src/linux-source-* 2>/dev/null
    exit 1
fi
echo "  Source:  $SRC_TARBALL"

echo "[3/5] Extracting virtio sound source ..."
mkdir -p "$WORKDIR/ksrc"
case "$SRC_TARBALL" in
    *.xz)  xz -dc "$SRC_TARBALL"  | tar xf - -C "$WORKDIR/ksrc" --strip-components=1 --wildcards '*/sound/virtio/*' ;;
    *.gz)  gzip -dc "$SRC_TARBALL" | tar xf - -C "$WORKDIR/ksrc" --strip-components=1 --wildcards '*/sound/virtio/*' ;;
    *.zst) zstd -dc "$SRC_TARBALL" | tar xf - -C "$WORKDIR/ksrc" --strip-components=1 --wildcards '*/sound/virtio/*' ;;
    *)     tar xf "$SRC_TARBALL" -C "$WORKDIR/ksrc" --strip-components=1 --wildcards '*/sound/virtio/*' ;;
esac

VIRTIO_SND_SRC="$WORKDIR/ksrc/sound/virtio"
if [ ! -d "$VIRTIO_SND_SRC" ]; then
    echo "Error: sound/virtio/ not found in kernel source." >&2
    exit 1
fi

echo "  Source files:"
ls "$VIRTIO_SND_SRC/"

echo "[4/5] Building virtio_snd.ko ..."
BUILD_DIR="$WORKDIR/build"
mkdir -p "$BUILD_DIR"

cp "$VIRTIO_SND_SRC"/*.c "$BUILD_DIR/"
cp "$VIRTIO_SND_SRC"/*.h "$BUILD_DIR/"

# The kernel source Makefile for sound/virtio/ references obj-$(CONFIG_SND_VIRTIO).
# Copy it and ensure CONFIG_SND_VIRTIO=m is passed to make.
if [ -f "$VIRTIO_SND_SRC/Makefile" ]; then
    cp "$VIRTIO_SND_SRC/Makefile" "$BUILD_DIR/Makefile"
else
    # Fallback: construct a Makefile matching upstream sound/virtio/Makefile
    cat > "$BUILD_DIR/Makefile" << 'MKEOF'
obj-m += virtio_snd.o
virtio_snd-objs := virtio_card.o virtio_ctl_msg.o virtio_pcm.o \
                    virtio_pcm_msg.o virtio_pcm_ops.o
MKEOF
fi

echo "  Makefile:"
cat "$BUILD_DIR/Makefile"
echo ""

make -C "$HEADER_DIR" M="$BUILD_DIR" modules CONFIG_SND_VIRTIO=m

echo "[5/5] Collecting output ..."
if [ -f "$BUILD_DIR/virtio_snd.ko" ]; then
    cp "$BUILD_DIR/virtio_snd.ko" "$OUTDIR/virtio_snd.ko"
    echo ""
    echo "=== Success ==="
    echo "  Output: $OUTDIR/virtio_snd.ko"
    file "$OUTDIR/virtio_snd.ko"
    ls -lh "$OUTDIR/virtio_snd.ko"
else
    echo "Error: virtio_snd.ko was not produced." >&2
    echo "  Build directory contents:"
    ls -la "$BUILD_DIR"/ 2>/dev/null
    exit 1
fi
