#!/usr/bin/env python3

from __future__ import annotations

import io
import shutil
import subprocess
import tarfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent
PKG_ROOT = ROOT / "package" / "network-esp32-ble-printer"
FILES_ROOT = PKG_ROOT / "files"
DIST_DIR = ROOT / "dist"
BUILD_DIR = ROOT / ".ipk-build"

PACKAGE_NAME = "network-esp32-ble-printer"
PACKAGE_VERSION = "0.2.0-1"
ARCH = "all"
OUT_NAME = f"{PACKAGE_NAME}_{PACKAGE_VERSION}_{ARCH}.ipk"


def build_control_tar(control_dir: Path) -> bytes:
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w:gz", format=tarfile.USTAR_FORMAT) as tar:
        for child in sorted(control_dir.iterdir()):
            tar.add(child, arcname=child.name, recursive=True)
    return data.getvalue()


def build_data_tar(files_dir: Path) -> bytes:
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w:gz", format=tarfile.USTAR_FORMAT) as tar:
        for child in sorted(files_dir.rglob("*")):
            if "__pycache__" in child.parts:
                continue
            if child.suffix == ".pyc":
                continue
            tar.add(child, arcname=str(child.relative_to(files_dir)), recursive=False)
    return data.getvalue()


def main() -> None:
    shutil.rmtree(BUILD_DIR, ignore_errors=True)
    DIST_DIR.mkdir(parents=True, exist_ok=True)
    control_dir = BUILD_DIR / "control"
    control_dir.mkdir(parents=True, exist_ok=True)

    control_text = "\n".join(
        [
            f"Package: {PACKAGE_NAME}",
            f"Version: {PACKAGE_VERSION}",
            "Depends: python3, python3-pillow, procd",
            "Source: local",
            "Section: net",
            "Priority: optional",
            f"Architecture: {ARCH}",
            "Maintainer: OpenAI Codex",
            "Description: NESPi-oriented OpenWrt image bridge for the Mervyns ESP32 BLE label printer.",
            "",
        ]
    )
    (control_dir / "control").write_text(control_text, encoding="utf-8", newline="\n")

    control_tar = build_control_tar(control_dir)
    data_tar = build_data_tar(FILES_ROOT)
    debian_binary = b"2.0\n"
    out_path = DIST_DIR / OUT_NAME
    control_tar_path = BUILD_DIR / "control.tar.gz"
    data_tar_path = BUILD_DIR / "data.tar.gz"
    debian_binary_path = BUILD_DIR / "debian-binary"
    control_tar_path.write_bytes(control_tar)
    data_tar_path.write_bytes(data_tar)
    debian_binary_path.write_bytes(debian_binary)

    if out_path.exists():
        out_path.unlink()

    subprocess.run(
        [
            "tar.exe",
            "--format",
            "ar",
            "-c",
            "-f",
            str(out_path),
            "-C",
            str(BUILD_DIR),
            "debian-binary",
            "control.tar.gz",
            "data.tar.gz",
        ],
        check=True,
    )

    print(out_path)


if __name__ == "__main__":
    main()
