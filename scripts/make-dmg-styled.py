#!/usr/bin/env python3
"""Build a styled DMG with large icons and a drag-and-drop arrow background.

Workarounds for macOS 26 Tahoe:
  - Uses APFS instead of HFS+ (HFS+ external media has mounting bugs)
  - Uses a temporary volume name during build to avoid Gatekeeper blocking
    ditto when the .app name matches the volume name
  - Uses Finder AppleScript to configure the view (Finder ignores .DS_Store
    files not written by itself)
"""

import os
import subprocess
import sys
import tempfile
import time


def hdiutil(*args):
    r = subprocess.run(
        ["hdiutil"] + list(args),
        capture_output=True, text=True,
    )
    if r.returncode != 0:
        raise RuntimeError(f"hdiutil {args[0]} failed: {r.stderr.strip()}")
    return r.stdout


def applescript(script):
    r = subprocess.run(["osascript", "-e", script], capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  AppleScript warning: {r.stderr.strip()}")
    return r.stdout.strip()


def main():
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} <app_path> <volume_name> <output.dmg>")
        sys.exit(1)

    app_path = os.path.abspath(sys.argv[1])
    volume_name = sys.argv[2]
    output_dmg = os.path.abspath(sys.argv[3])

    if not os.path.isdir(app_path):
        print(f"Error: {app_path} not found")
        sys.exit(1)

    script_dir = os.path.dirname(os.path.abspath(__file__))
    build_dir = os.path.join(script_dir, os.pardir, "build")
    os.makedirs(build_dir, exist_ok=True)
    bg_tiff = os.path.join(build_dir, "dmg-background.tiff")
    if not os.path.exists(bg_tiff):
        subprocess.run([sys.executable, os.path.join(script_dir, "make-dmg-background.py"), bg_tiff], check=True)
    with open(bg_tiff, "rb") as f:
        bg_data = f.read()

    app_basename = os.path.basename(app_path)
    # Temporary volume name avoids Gatekeeper blocking ditto when
    # .app name matches volume name (e.g. TenBox.app -> /Volumes/TenBox/)
    tmp_volname = volume_name + " _build"

    with tempfile.TemporaryDirectory() as tmpdir:
        sparse = os.path.join(tmpdir, "build.sparseimage")

        # ── Step 1: Create APFS sparse image ────────────────────────────
        print("  [1/5] Creating APFS sparse image...")
        hdiutil("create", "-size", "200m", "-fs", "APFS",
                "-volname", tmp_volname, "-type", "SPARSE", sparse)

        # ── Step 2: Mount and populate ──────────────────────────────────
        print("  [2/5] Populating disk image...")
        out = hdiutil("attach", sparse, "-noverify", "-noautoopen", "-owners", "off")
        mount_point = None
        device = None
        for line in out.strip().split("\n"):
            cols = line.split("\t")
            if len(cols) >= 3:
                mount_point = cols[-1].strip()
            c = line.split()
            if c and c[0].startswith("/dev/disk"):
                device = c[0]

        if not mount_point or not os.path.isdir(mount_point):
            raise RuntimeError(f"Could not mount sparse image: {out}")

        try:
            subprocess.run(
                ["ditto", app_path, os.path.join(mount_point, app_basename)],
                check=True,
            )
            os.symlink("/Applications", os.path.join(mount_point, "Applications"))

            bg_dir = os.path.join(mount_point, ".background")
            os.makedirs(bg_dir, exist_ok=True)
            bg_path = os.path.join(bg_dir, "background.tiff")
            with open(bg_path, "wb") as f:
                f.write(bg_data)

            # ── Step 3: Rename volume to final name ─────────────────────
            print("  [3/5] Renaming volume...")
            # Find the APFS volume device (not the container)
            mount_out = subprocess.run(["mount"], capture_output=True, text=True).stdout
            apfs_dev = None
            for line in mount_out.split("\n"):
                if mount_point in line:
                    apfs_dev = line.split()[0]
                    break
            if apfs_dev:
                r = subprocess.run(
                    ["diskutil", "rename", apfs_dev, volume_name],
                    capture_output=True, text=True,
                )
                if r.returncode != 0:
                    print(f"  Warning: rename failed: {r.stderr.strip()}")
                mount_point = f"/Volumes/{volume_name}"
                if not os.path.isdir(mount_point):
                    print(f"  Warning: {mount_point} not found after rename")

            # Give Finder time to notice the new volume
            time.sleep(2)

            # ── Step 4: Configure Finder view via AppleScript ───────────
            print("  [4/5] Configuring Finder view...")
            bg_ref = 'file ".background:background.tiff"'
            applescript(f'''
                tell application "Finder"
                    tell disk "{volume_name}"
                        open
                        set current view of container window to icon view
                        set toolbar visible of container window to false
                        set statusbar visible of container window to false
                        set the bounds of container window to {{100, 100, 760, 500}}
                        set viewOptions to the icon view options of container window
                        set arrangement of viewOptions to not arranged
                        set icon size of viewOptions to 128
                        set text size of viewOptions to 14
                        set background picture of viewOptions to {bg_ref}
                        set position of item "{app_basename}" of container window to {{170, 160}}
                        set position of item "Applications" of container window to {{490, 160}}
                        close
                        open
                        delay 2
                        close
                    end tell
                end tell
            ''')

            # Wait for Finder to flush .DS_Store
            time.sleep(2)

            # Verify
            ds_path = os.path.join(mount_point, ".DS_Store")
            if os.path.exists(ds_path):
                print(f"  .DS_Store written by Finder ({os.path.getsize(ds_path)} bytes)")
            else:
                print("  Warning: Finder did not write .DS_Store")

            subprocess.run(["sync", "--file-system", mount_point], check=True)
        finally:
            hdiutil("detach", device)

        # ── Step 5: Shrink and convert ──────────────────────────────────
        print("  [5/5] Finalizing DMG...")
        hdiutil("resize", "-sectors", "min", sparse)

        if os.path.exists(output_dmg):
            os.remove(output_dmg)
        hdiutil("convert", sparse, "-format", "UDZO",
                "-imagekey", "zlib-level=9", "-o", output_dmg)

    size_mb = os.path.getsize(output_dmg) / (1024 * 1024)
    print(f"  Done: {output_dmg} ({size_mb:.1f} MB)")


if __name__ == "__main__":
    main()
