#!/usr/bin/env python3
"""Check the target compiler's stack accounting and rendering call sites.

Run after `make build`. Host graphics tests use the host's much larger stack;
this check catches the nested ESP32-P4 render frames that those tests miss.
"""
from pathlib import Path
import json
import re
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    build = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build/tanmatsu"
    object_path = build / "esp-idf/main/CMakeFiles/__idf_main.dir/ui/ui.c.obj"
    stack_path = object_path.with_suffix(".su")
    if not stack_path.is_file():
        raise SystemExit("Run make build first to generate target UI stack accounting.")
    frames = {}
    for line in stack_path.read_text().splitlines():
        location, size, kind = line.split("\t")
        name = location.rsplit(":", 1)[1]
        frames[name] = int(size)
        if name in {"ui_render", "ui_draw_search", "ui_draw_button_legend", "ui_status_task", "ui_draw"}:
            assert kind == "static", f"Unbounded stack frame: {name} ({kind})"
    # Both tasks have 8 KiB stacks. Reserve at least 6 KiB for graphics, library
    # calls and scheduler overhead rather than filling that stack with records.
    about_object = build / "esp-idf/main/CMakeFiles/__idf_main.dir/ui/about_view.c.su"
    about_frames = {}
    for line in about_object.read_text().splitlines():
        location, size, kind = line.split("\t")
        about_frames[location.rsplit(":", 1)[1]] = int(size)
        assert kind == "static", f"Unbounded About frame: {kind}"
    about_stack = about_frames["ui_about_draw"] + about_frames.get("about_row", 0)
    settings_object = build / "esp-idf/main/CMakeFiles/__idf_main.dir/ui/settings.c.su"
    settings_stack = next(int(line.split("\t")[1]) for line in settings_object.read_text().splitlines()
                          if ":ui_settings_draw\t" in line)
    rendering = frames["ui_render"] + max(
        frames["ui_draw_search"] + frames["ui_draw_button_legend"],
        frames["ui_draw_remove_dialog"] + frames["ui_draw_button_legend"],
        frames["ui_draw_session_screen"], frames["ui_draw_exit_confirmation"],
        frames["ui_draw_startup"] + max(frames["ui_draw_status_bar"], settings_stack),
        frames["ui_draw_now_playing"] +
        max(frames["ui_draw_station_logo"], frames["ui_draw_marquee"], frames["ui_first_line"]), about_stack
    )
    total = frames["ui_status_task"] + max(rendering, frames["ui_sync_now_playing"])
    assert total <= 2048, f"UI rendering owns {total} bytes of stack (budget 2048)"
    assert frames["ui_draw"] <= 128, "Input-side redraw request grew a rendering frame"

    station_stack = build / "esp-idf/main/CMakeFiles/__idf_main.dir/radio/stations.c.su"
    catalog_frames = {}
    for line in station_stack.read_text().splitlines():
        location, size, kind = line.split("\t")
        catalog_frames[location.rsplit(":", 1)[1]] = int(size)
    removal = frames["ui_handle_navigation"] + frames["ui_remove_navigation"] + max(
        frames["ui_refresh_preview"],
        catalog_frames["stations_remove"] + catalog_frames["parse_line"] +
        catalog_frames["parse_fields"] + catalog_frames["parse_bitrate"]
    )
    assert removal <= 3072, f"Removal path owns {removal} bytes of input stack (budget 3072)"

    commands = json.loads((build / "compile_commands.json").read_text())
    ui_command = next(item for item in commands if item["file"].endswith("/main/ui/ui.c"))
    compiler = Path(shlex.split(ui_command["command"])[0])
    objdump = compiler.with_name(compiler.name.removesuffix("-gcc") + "-objdump")
    result = subprocess.run([str(objdump), "-dr", str(object_path)],
                            check=True, capture_output=True, text=True)
    callers = set()
    caller = None
    for line in result.stdout.splitlines():
        section = re.match(r"Disassembly of section \.text\.([^:]+):", line)
        if section:
            caller = section.group(1)
        call = re.search(r"R_RISCV_CALL(?:_PLT)?\s+ui_render(?:\s|$)", line)
        if call:
            callers.add(caller)
    assert callers == {"ui_start", "ui_status_task"}, f"Unexpected rendering callers: {callers}"
    print(f"PASS: target UI render frames total {total}/2048 bytes; "
          f"removal frames {removal}/3072 bytes; "
          "input only queues redraws; rendering is confined to startup/UI task")


if __name__ == "__main__":
    main()
