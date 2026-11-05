#!/usr/bin/env python3
"""Create a source-and-release snapshot for a new Git repository."""
from pathlib import Path
import hashlib
import shutil
import sys
import tempfile

from check_privacy import check_tree

ROOT = Path(__file__).resolve().parents[1]
FILES = (
    ".editorconfig", ".gitignore", "README.md", "ARCHITECTURE.md",
    "CHANGELOG.md", "CMakeLists.txt", "Makefile", "dependencies.lock",
    "metadata.json", "LICENSE",
    "sdkconfig_tanmatsu", "icon16.png", "icon32.png", "icon64.png",
    "releases/TanRadio.bin", "releases/TanRadio.bin.sha256", "releases/README.md",
)
DIRECTORIES = ("main", "local-components", "partition_tables", "sdkconfigs", "tools")


def main():
    destination = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "dist/TanRadio-source").resolve()
    if destination.exists() or destination.is_symlink():
        raise SystemExit(f"Refusing to overwrite {destination}. Move the existing folder before exporting again.")
    if destination == ROOT or any(destination.is_relative_to(ROOT / name) for name in DIRECTORIES + ("releases",)):
        raise SystemExit("Choose an export folder outside the source directories, such as dist/TanRadio-source.")
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=".tanradio-export-", dir=destination.parent))
    try:
        for name in FILES:
            (temporary / name).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / name, temporary / name)
        for name in DIRECTORIES:
            shutil.copytree(ROOT / name, temporary / name,
                            ignore=shutil.ignore_patterns("__pycache__", "*.pyc", ".git", ".github"))
        findings = check_tree(temporary)
        if findings:
            raise SystemExit("Privacy check failed:\n" + "\n".join(findings))
        records = []
        for path in sorted(temporary.rglob("*")):
            if path.is_file():
                records.append(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(temporary).as_posix()}\n")
        (temporary / "SOURCE_MANIFEST.sha256").write_text("".join(records))
        temporary.rename(destination)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    print(f"Created {destination} ({len(records)} files plus SHA-256 manifest)")


if __name__ == "__main__":
    main()
