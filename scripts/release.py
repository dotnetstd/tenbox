"""
Release script: build MSI → upload to Alibaba Cloud OSS → update version.json.

Usage:
    python release.py
    python release.py --notes "Fixed a bug\\nImproved performance"

Prerequisites:
    pip install oss2 python-dotenv
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from datetime import date
from pathlib import Path

try:
    import oss2
except ImportError:
    print("ERROR: oss2 not installed. Run: pip install oss2")
    sys.exit(1)

try:
    from dotenv import load_dotenv
except ImportError:
    print("ERROR: python-dotenv not installed. Run: pip install python-dotenv")
    sys.exit(1)

SCRIPTS_DIR = Path(__file__).resolve().parent
PROJECT_DIR = SCRIPTS_DIR.parent
BUILD_DIR = PROJECT_DIR / "build" / "x64-release"
VERSION_JSON_PATH = PROJECT_DIR / "website" / "public" / "api" / "version.json"
CMAKELISTS_PATH = PROJECT_DIR / "CMakeLists.txt"
OSS_DOWNLOAD_BASE = "https://files.xiaozhi.me"


def extract_version() -> str:
    text = CMAKELISTS_PATH.read_text(encoding="utf-8")
    m = re.search(r"^project\s*\(\s*TenBox\s+VERSION\s+([\d.]+)", text, re.MULTILINE)
    if not m:
        print("ERROR: Could not extract version from CMakeLists.txt")
        sys.exit(1)
    return m.group(1)


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            h.update(chunk)
    return h.hexdigest()


def build_msi():
    print()
    print("=" * 40)
    print("  Step 1: Building MSI installer")
    print("=" * 40)
    bat = SCRIPTS_DIR / "build-msi.bat"
    result = subprocess.run([str(bat)], shell=True, cwd=str(SCRIPTS_DIR))
    if result.returncode != 0:
        print("ERROR: build-msi.bat failed.")
        sys.exit(1)


def upload_to_oss(msi_path: Path, oss_key: str):
    print()
    print("=" * 40)
    print("  Step 3: Uploading to OSS")
    print("=" * 40)

    region = os.environ["OSS_REGION"]
    access_key_id = os.environ["OSS_ACCESS_KEY_ID"]
    access_key_secret = os.environ["OSS_ACCESS_KEY_SECRET"]
    bucket_name = os.environ["OSS_BUCKET_NAME"]

    endpoint = f"https://{region}.aliyuncs.com"
    auth = oss2.Auth(access_key_id, access_key_secret)
    bucket = oss2.Bucket(auth, endpoint, bucket_name)

    file_size = msi_path.stat().st_size
    print(f"  Uploading {msi_path.name} ({file_size / 1024 / 1024:.1f} MB)...")

    oss2.resumable_upload(bucket, oss_key, str(msi_path),
                          multipart_threshold=10 * 1024 * 1024,
                          part_size=2 * 1024 * 1024,
                          num_threads=4)

    print(f"  Uploaded: oss://{bucket_name}/{oss_key}")


def update_version_json(version: str, download_url: str, sha256: str, release_notes: str):
    print()
    print("=" * 40)
    print("  Step 4: Updating version.json")
    print("=" * 40)

    existing = {}
    if VERSION_JSON_PATH.exists():
        existing = json.loads(VERSION_JSON_PATH.read_text(encoding="utf-8"))

    existing.update({
        "latest_version": version,
        "download_url": download_url,
        "release_notes": release_notes,
        "release_date": date.today().isoformat(),
        "sha256": sha256,
    })

    VERSION_JSON_PATH.write_text(
        json.dumps(existing, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(f"  Updated: {VERSION_JSON_PATH}")


def parse_args():
    parser = argparse.ArgumentParser(description="Build and release TenBox MSI installer.")
    parser.add_argument("--notes", "-n", type=str, default=None,
                        help="Release notes (use \\n for newlines). If omitted, prompts interactively.")
    return parser.parse_args()


def main():
    args = parse_args()
    load_dotenv(SCRIPTS_DIR / ".env")

    required_vars = [
        "OSS_REGION", "OSS_ACCESS_KEY_ID", "OSS_ACCESS_KEY_SECRET",
        "OSS_BUCKET_NAME", "OSS_TENBOX_RELEASES_DIR",
    ]
    missing = [v for v in required_vars if not os.environ.get(v)]
    if missing:
        print(f"ERROR: Missing env vars: {', '.join(missing)}")
        print("Copy .env.sample to .env and fill in values.")
        sys.exit(1)

    # Step 1: Build MSI
    build_msi()

    # Extract version and locate MSI
    version = extract_version()
    msi_name = f"TenBox_{version}.msi"
    msi_path = BUILD_DIR / msi_name

    if not msi_path.exists():
        print(f"ERROR: MSI file not found: {msi_path}")
        sys.exit(1)

    # Step 2: SHA256
    print()
    print("=" * 40)
    print("  Step 2: Calculating SHA256")
    print("=" * 40)
    sha256 = sha256_file(msi_path)
    print(f"  SHA256: {sha256}")

    # Step 3: Upload to OSS
    releases_dir = os.environ["OSS_TENBOX_RELEASES_DIR"]
    oss_key = f"{releases_dir}/{msi_name}"
    upload_to_oss(msi_path, oss_key)

    # Step 4: Update version.json
    download_url = f"{OSS_DOWNLOAD_BASE}/{oss_key}"

    if args.notes is not None:
        release_notes = args.notes
    else:
        release_notes = input("\nEnter release notes (use \\n for newlines): ").strip()
    if not release_notes:
        release_notes = "Bug fixes and improvements"

    update_version_json(version, download_url, sha256, release_notes)

    # Done
    print()
    print("=" * 40)
    print(f"  Release v{version} complete!")
    print("=" * 40)
    print(f"  MSI:      {msi_path}")
    print(f"  OSS:      {download_url}")
    print(f"  SHA256:   {sha256}")
    print(f"  Date:     {date.today().isoformat()}")
    print()
    print("  Next: commit and push website/public/api/version.json")
    print("  to trigger website redeployment.")
    print("=" * 40)


if __name__ == "__main__":
    main()
