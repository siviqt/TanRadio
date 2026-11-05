#!/usr/bin/env python3
"""Generate firmware release constants from the repository metadata.json."""
import argparse
import json
from pathlib import Path
import re


def release_info(path):
    metadata = json.loads(Path(path).read_text())
    version = metadata.get("version")
    if not isinstance(version, str) or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?", version):
        raise ValueError("metadata.json must contain a semantic application version")
    applications = [app for app in metadata.get("application", [])
                    if app.get("type") == "appfs" and "tanmatsu" in app.get("targets", [])]
    if len(applications) != 1:
        raise ValueError("metadata.json must contain exactly one Tanmatsu AppFS application")
    revision = applications[0].get("revision")
    if type(revision) is not int or not 0 <= revision <= 65535:
        raise ValueError("AppFS revision must be an integer from 0 to 65535")
    return version, revision


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--metadata", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--print-build-version", action="store_true")
    action.add_argument("--print-revision", action="store_true")
    parser.add_argument("--bsp", default="local")
    parser.add_argument("--lcd", default="local")
    parser.add_argument("--codec", default="local")
    args = parser.parse_args()
    version, revision = release_info(args.metadata)
    if args.print_build_version:
        build_version = f"{version}-r{revision}"
        if len(build_version) > 31:
            parser.error("Firmware build version exceeds ESP-IDF's 31-character limit")
        print(build_version)
        return
    if args.print_revision:
        print(revision)
        return
    if args.output is None:
        parser.error("--output is required when generating the header")
    constants = {"TANRADIO_APP_VERSION": version, "TANRADIO_BSP_VERSION": args.bsp or "local",
                 "TANRADIO_LCD_VERSION": args.lcd or "local", "TANRADIO_CODEC_VERSION": args.codec or "local"}
    content = "/* Generated from metadata.json and the resolved build components. */\n#pragma once\n"
    content += "".join(f"#define {name} {json.dumps(value)}\n" for name, value in constants.items())
    content += f"#define TANRADIO_APP_REVISION {revision}u\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text() != content:
        args.output.write_text(content)


if __name__ == "__main__":
    main()
