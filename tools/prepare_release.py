#!/usr/bin/env python3
"""Copy a verified ordinary firmware bundle into the versioned release directory."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


def sha(data):
    return hashlib.sha256(data).hexdigest()


def prepare(bundle, output):
    root = Path(__file__).resolve().parent.parent
    version = (root / "version.txt").read_text(encoding="utf-8-sig").strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?", version):
        raise ValueError("invalid version.txt")
    subprocess.run([sys.executable, str(root / "tools/verify_firmware.py"), str(bundle)], check=True)
    sources = json.loads((bundle / "source-hashes.json").read_text(encoding="utf-8-sig"))
    for name, digest in sources.items():
        path = (root / name).resolve()
        if not path.is_relative_to(root) or sha(path.read_bytes()) != digest:
            raise ValueError(f"bundle source mismatch: {name}")
    app = (bundle / "FoloToy-AI-Passport.bin").read_bytes()
    full = (bundle / "FoloToy-AI-Passport-full.bin").read_bytes()
    if version.encode() + b"\0" not in app:
        raise ValueError("APP does not contain the selected version")
    if any(marker in app for marker in (b"POWER_R80_DONE pass=", b"ABOUT_R79_DONE")):
        raise ValueError("diagnostic firmware cannot be released")
    # A distributable merged image must contain no captured device data in gaps.
    args = json.loads((bundle / "flasher_args.json").read_text(encoding="utf-8-sig"))
    cursor = 0
    for offset, name in sorted(args["flash_files"].items(), key=lambda item: int(item[0], 0)):
        offset = int(offset, 0)
        image = (bundle / name).read_bytes()
        if offset < cursor or full[cursor:offset] != b"\xff" * (offset - cursor):
            raise ValueError("overlapping images or non-blank merged padding")
        if full[offset:offset + len(image)] != image:
            raise ValueError(f"merged image mismatch: {name}")
        cursor = offset + len(image)
    if full[cursor:] != b"\xff" * (len(full) - cursor):
        raise ValueError("non-blank merged trailing padding")
    files = {}
    prefix = f"ZION-Feishu-{version}"
    for kind, data, address in (("full", full, "0x0"), ("app", app, "0x10000")):
        files[f"{prefix}-{kind}.bin"] = (data, address)
    destination = output / version
    # Reject conflicting existing versions before writing anything.
    for name, (data, _) in files.items():
        target = destination / name
        if target.exists() and target.read_bytes() != data:
            raise ValueError(f"version already has a different artifact: {target}")
    destination.mkdir(parents=True, exist_ok=True)
    manifest = {"product": "ZION Feishu Smart Client", "version": version,
                "chip": "esp32c3", "flash_size": "8MB",
                "artifacts": [], "source_hashes": sources}
    for name, (data, address) in files.items():
        (destination / name).write_bytes(data)
        manifest["artifacts"].append({"file": name, "bytes": len(data),
                                      "sha256": sha(data), "flash_address": address})
    (destination / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Release files verified: {destination}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parent.parent / "release")
    options = parser.parse_args()
    try:
        prepare(options.bundle.resolve(), options.output.resolve())
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Release preparation failed: {error}\n")
