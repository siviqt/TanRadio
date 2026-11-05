#!/usr/bin/env python3
"""Prepare separate Tanmatsu repository and personal Git source packages."""
from pathlib import Path
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import zipfile

from check_privacy import check_tree
from generate_build_info import release_info

ROOT = Path(__file__).resolve().parents[1]
SLUG = "com.github.siviqt.tanradio"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    version, revision = release_info(ROOT / "metadata.json")
    label = f"TanRadio-{version}-r{revision}"
    destination = ROOT / "dist" / label
    if destination.exists():
        raise SystemExit(f"Refusing to overwrite {destination}; move it before packaging again.")
    image = ROOT / "releases/TanRadio.bin"
    data = image.read_bytes()
    # ESP-IDF app description follows the 24-byte image and 8-byte segment headers.
    if len(data) < 288 or struct.unpack_from("<I", data, 32)[0] != 0xABCD5432:
        raise SystemExit("The release is not an ESP-IDF application image")
    actual_version = data[48:80].split(b"\0", 1)[0].decode()
    if actual_version != f"{version}-r{revision}":
        raise SystemExit("Release image version differs from metadata.json; run make release first")
    if (ROOT / "releases/TanRadio.bin.sha256").read_text().split() != [digest(image), "TanRadio.bin"]:
        raise SystemExit("Release image checksum differs from its sidecar")
    metadata = json.loads((ROOT / "metadata.json").read_text())
    files = {"metadata.json", "LICENSE", *metadata["icon"].values()}
    for app in metadata["application"]:
        files.add(app["executable"])
        files.update(asset["source_file"] for asset in app.get("assets", []))
    for name in files:
        if Path(name).name != name:
            raise SystemExit(f"Expected a flat repository package filename: {name}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=".tanradio-package-", dir=destination.parent))
    try:
        repository = temporary / "repository" / SLUG
        repository.mkdir(parents=True)
        for name in sorted(files):
            shutil.copy2(image if name == "TanRadio.bin" else ROOT / name, repository / name)
        shutil.copy2(ROOT / "tools/repository_README.md", repository / "README.md")
        source = temporary / "TanRadio-source"
        subprocess.run([sys.executable, str(ROOT / "tools/export_source.py"), str(source)], check=True)
        findings = check_tree(temporary)
        if findings:
            raise SystemExit("Privacy check failed:\n" + "\n".join(findings))
        with zipfile.ZipFile(temporary / f"{label}-repository.zip", "w", zipfile.ZIP_DEFLATED) as archive:
            for path in sorted(repository.rglob("*")):
                if path.is_file():
                    archive.write(path, path.relative_to(repository.parent))
        with tarfile.open(temporary / f"{label}-source.tar.gz", "w:gz") as archive:
            archive.add(source, arcname="TanRadio-source")
        # The working checkout is the source of truth. Keep delivery archives,
        # without leaving additional unpacked source/package trees beside it.
        shutil.rmtree(repository.parent)
        shutil.rmtree(source)
        (temporary / "README.md").write_text(
            f"# TanRadio {version}, revision {revision}\n\n"
            f"Extract `{label}-repository.zip` and copy `{SLUG}/` into the Tanmatsu "
            "application repository, replacing that application's folder.\n\n"
            f"`{label}-source.tar.gz` contains the `TanRadio-source/` snapshot for a "
            "personal Git repository. It includes the matching binary, local component patches, "
            "licenses, pinned dependencies, tests and build instructions.\n\n"
            "Verify the complete package with `sha256sum -c SHA256SUMS`. "
            "No device upload, Git commit or remote publication is performed by packaging.\n\n"
            f"Firmware build: `{actual_version}`; image SHA-256: `{digest(image)}`.\n")
        paths = sorted(path for path in temporary.rglob("*") if path.is_file())
        (temporary / "SHA256SUMS").write_text("".join(
            f"{digest(path)}  {path.relative_to(temporary).as_posix()}\n" for path in paths))
        temporary.rename(destination)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    print(f"Prepared repository upload and personal Git source: {destination}")


if __name__ == "__main__":
    main()
