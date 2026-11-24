"""Describe the exact macOS and Windows release ZIPs before signing their manifest."""
import argparse
import hashlib
import json
from pathlib import Path
import re

PLATFORMS = ("macos-x86_64", "windows-x64")


def describe(folder, version, build, tag):
    if not re.fullmatch(r"(?:0|[1-9][0-9]{0,5})\.(?:0|[1-9][0-9]{0,5})\.(?:0|[1-9][0-9]{0,5})", version):
        raise ValueError("Expected a three-component release version")
    if not 0 < build <= 65535 or tag not in (f"v{version}-rc.{build}", f"v{version}"):
        raise ValueError("Tag or build does not match this release")
    assets = {}
    for platform in PLATFORMS:
        name = f"KEEFI-{version}-{build}-{platform}.zip"
        archive = folder / name
        if archive.is_symlink() or not archive.is_file():
            raise ValueError(f"Missing release file: {name}")
        digest = hashlib.sha256()
        size = 0
        with archive.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                size += len(chunk)
                digest.update(chunk)
        if not 0 < size <= 512 * 1024 * 1024:
            raise ValueError(f"Archive is empty or exceeds the client download limit: {name}")
        assets[platform] = {"name": name, "sha256": digest.hexdigest(), "size": size}
    return {"schema": 1, "version": version, "build": build, "tag": tag, "assets": assets}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--build", required=True, type=int)
    parser.add_argument("--tag", required=True)
    args = parser.parse_args()
    data = describe(args.directory, args.version, args.build, args.tag)
    output = args.directory / "keefi-update.json"
    with output.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(data, stream, sort_keys=True, indent=2)
        stream.write("\n")
