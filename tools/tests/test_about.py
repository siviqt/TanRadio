#!/usr/bin/env python3
"""Exercise actual Hyper routing, welcome modal behavior and metadata generation."""
from pathlib import Path
import importlib.util
import json
import os
import subprocess
import tempfile

from test_controls import STUBS
from test_display_updates import function
from test_exit import HARNESS

ROOT = Path(__file__).resolve().parents[2]

CHECKS = r'''
static void reset(void) {
    assert(!locks);
    ui_about_open = false; ui_exit_state = UI_EXIT_IDLE; ui_display_enabled = true;
    ui_exit_presented = ui_exit_started = ui_services_started = false;
    atomic_store(&ui_services_state, UI_SERVICES_READY);
    redraws = toggles = routed = volumes = exits = restarts = delays = 0;
    about_frames = splash_frames = confirm_frames = blits = event_count = event_index = 0;
    for (unsigned i = 0; i < sizeof(hardware_down)/sizeof(*hardware_down); i++) hardware_down[i] = false;
}
static void nav(bsp_input_navigation_key_t key, bool state) {
    events[event_count++] = (bsp_input_event_t){.type = INPUT_EVENT_TYPE_NAVIGATION,
        .args_navigation = {.key = key, .state = state}};
}
static void space(void) {
    events[event_count++] = (bsp_input_event_t){.type = INPUT_EVENT_TYPE_KEYBOARD,
        .args_keyboard = {.ascii = ' '}};
}
int main(void) {
    reset(); input_keys_t keys; bsp_input_navigation_key_t repeated;
    input_keys_init(&keys, 500, 100);
    assert(input_keys_event(&keys, BSP_INPUT_NAVIGATION_KEY_DOWN, true, 0));
    assert(input_keys_event(&keys, BSP_INPUT_NAVIGATION_KEY_SUPER, true, 10));
    ui_display_enabled = false;
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_SUPER);
    assert(ui_about_open && ui_display_enabled && toggles == 1 && !routed);
    assert(!input_keys_due(&keys, 10000, &repeated));
    assert(!input_keys_event(&keys, BSP_INPUT_NAVIGATION_KEY_SUPER, true, 20));
    assert(ui_about_open);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_RETURN);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F4);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F5);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_BACKSPACE);
    assert(ui_about_open && !routed && !exits);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_VOLUME_UP);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_VOLUME_DOWN);
    assert(volumes == 2 && ui_about_open);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F6);
    assert(routed == 1 && ui_about_open);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F1);
    assert(ui_exit_state == UI_EXIT_CONFIRM && ui_about_open);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_SUPER);
    assert(ui_exit_state == UI_EXIT_CONFIRM && ui_about_open);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_ESC);
    assert(ui_exit_state == UI_EXIT_IDLE && ui_about_open);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_F2);
    assert(!ui_about_open && routed == 1);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_SUPER);
    input_dispatch_navigation(BSP_INPUT_NAVIGATION_KEY_SUPER);
    assert(!ui_about_open && routed == 1);

    /* The welcome queue gets the BSP's Super down/up pulse after a Hyper tap.
     * Space while About is open cannot start Wi-Fi/audio or skip the popup. */
    reset(); nav(BSP_INPUT_NAVIGATION_KEY_SUPER, true);
    nav(BSP_INPUT_NAVIGATION_KEY_SUPER, false); space();
    nav(BSP_INPUT_NAVIGATION_KEY_ESC, true); space();
    assert(ui_show_welcome() && event_index == event_count);
    assert(!ui_about_open && about_frames == 1 && !routed && !exits);
    assert(!audio_shutdowns && !restarts);

    /* Canceling F1 restores About; another Hyper tap returns to welcome. */
    reset(); nav(BSP_INPUT_NAVIGATION_KEY_SUPER, true);
    nav(BSP_INPUT_NAVIGATION_KEY_SUPER, false);
    nav(BSP_INPUT_NAVIGATION_KEY_F1, true);
    nav(BSP_INPUT_NAVIGATION_KEY_F2, true); space();
    nav(BSP_INPUT_NAVIGATION_KEY_SUPER, true); space();
    assert(ui_show_welcome() && event_index == event_count && !ui_about_open);
    assert(about_frames == 2 && confirm_frames == 1 && !splash_frames);
    puts("PASS: Hyper toggling/non-repeat, modal actions, volume, exit cancellation, welcome Space gating and display wake");
}
'''


def main():
    harness = HARNESS.replace("static void ui_about_draw(void *buffer) { (void)buffer; }",
        "static unsigned about_frames; static void ui_about_draw(void *buffer) { (void)buffer; about_frames++; }")
    harness += function(ROOT / "main/ui/settings.c", "bool ui_settings_edit(")
    harness += function(ROOT / "main/ui/ui.c", "bool ui_handle_exit_navigation(")
    harness += function(ROOT / "main/input/input.c", "static void input_dispatch_navigation(")
    harness += function(ROOT / "main/ui/ui.c", "static void ui_complete_exit(")
    harness += function(ROOT / "main/ui/ui.c", "static bool ui_welcome_event(")
    harness += function(ROOT / "main/ui/ui.c", "bool ui_show_welcome(")
    with tempfile.TemporaryDirectory(prefix="tanradio-about-") as directory:
        work = Path(directory)
        for name in ("esp_err.h", "freertos/FreeRTOS.h", "freertos/queue.h"):
            path = work / name
            path.parent.mkdir(exist_ok=True)
            path.write_text(STUBS)
        (work / "pax_gfx.h").write_text("typedef int pax_buf_t;\n")
        (work / "check.c").write_text(harness + CHECKS)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-I", str(work), "-I", str(ROOT / "main"),
            "-I", str(ROOT / "local-components/badgeteam__badge-bsp"),
            str(work / "check.c"), str(ROOT / "main/ui/about.c"),
            str(ROOT / "main/ui/exit.c"), str(ROOT / "main/input/keys.c"),
            "-o", str(work / "check")], check=True)
        subprocess.run([str(work / "check")], check=True,
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
        generator = ROOT / "tools/generate_build_info.py"
        spec = importlib.util.spec_from_file_location("build_info", generator)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        metadata = json.loads((ROOT / "metadata.json").read_text())
        version, revision = module.release_info(ROOT / "metadata.json")
        assert (version, revision) == (metadata["version"], metadata["application"][0]["revision"])
        subprocess.run(["python3", str(generator), "--metadata", str(ROOT / "metadata.json"),
            "--output", str(work / "info.h"), "--bsp", "2.0.0", "--lcd", "2.0.0", "--codec", "0.1.0"], check=True)
        text = (work / "info.h").read_text()
        assert f'#define TANRADIO_APP_VERSION "{version}"' in text
        assert f"#define TANRADIO_APP_REVISION {revision}u" in text
        assert '#define TANRADIO_BSP_VERSION "2.0.0"' in text
        assert '#define TANRADIO_LCD_VERSION "2.0.0"' in text
        for flag, expected in (("--print-build-version", f"{version}-r{revision}"),
                               ("--print-revision", str(revision))):
            result = subprocess.run(["python3", str(generator), "--metadata",
                str(ROOT / "metadata.json"), flag], check=True, capture_output=True, text=True)
            assert result.stdout.strip() == expected
        metadata["application"][0]["revision"] = True
        (work / "metadata.json").write_text(json.dumps(metadata))
        try:
            module.release_info(work / "metadata.json")
        except ValueError:
            pass
        else:
            raise AssertionError("Boolean revision accepted")
    print("PASS: About release constants come from metadata.json; invalid AppFS revision rejected")


if __name__ == "__main__":
    main()
