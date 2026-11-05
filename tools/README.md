# TanRadio Development Tools

This directory contains helper scripts used during development.

These tools are not part of the firmware image. Their purpose is to automate repetitive development tasks, enforce project standards, and improve reproducibility.

`make package` builds and checks production, refreshes the release image, then
runs `package_release.py` to prepare separate Tanmatsu repository and personal
Git source packages. It verifies the image version/checksum against metadata,
includes all declared repository assets, and records file hashes. The final
package keeps the ZIP/tar archives, README and checksums; unpacked staging trees
are removed so the working checkout remains the single source tree. Source exports
include only the current release. See `local-components/README.md` for the
BSP 2.0.0/LCD 2.0.0 integration, retained fixes and SDK snapshot provenance.

`generate_build_info.py` compiles the app version/revision from `metadata.json`
and component versions supplied by ESP-IDF into the About screen's constants.
Its print options also supply the firmware build version and BadgeLink revision,
so installation no longer hardcodes revision 1.
`test_about.py` covers actual Hyper routing, welcome Space gating, modal actions,
display wake, global volume/exit behavior and metadata validation. The target
stack check includes the About renderer. About rendering was also checked at
800 x 480 with the production PAX/Saira fonts.

After `make build`, run `make test` for all regression checks below,
including station removal and display scheduling. Run `make source-export` to
create a sharing snapshot through `export_source.py`. The exporter uses an
explicit source list, excludes generated/private directories, writes a SHA-256
manifest, and refuses to overwrite an existing destination. Before publishing the snapshot,
it checks for private machine paths, embedded private keys, and credential
assignments/tokens. Run `python3 tools/check_privacy.py` to check the working
source or pass an export directory to audit that snapshot. The check skips Git
history and generated build/dependency caches; public upstream credits and
certificate-authority contacts are retained. This is a heuristic check, so
review any newly added personal data before sharing.

Run `python3 tools/tests/test_audio_shutdown.py` to check audio control responsiveness
and shutdown ordering against a deliberately blocked PCM write. This host test
requires a C compiler (`cc`) and pthreads; it does not require a connected device.
It complements, but does not replace, on-device playback and exit checks.
It also checks 1% volume changes, saturation at 0/100%, and muted adjustment.

Run `python3 tools/tests/test_app_startup.py` to verify UI/input are active
before transport/catalog setup, atomic service publication, startup-task
failure handling and welcome exit. The exit test also covers shutdown while
background initialization is in progress.

Run `python3 tools/tests/test_network_startup.py` to verify transport/stack
initialization precedes scheduling saved-network attempts, a simulated slow
connection does not block the caller, and readiness/failure paths are safe.

After `make build` has fetched cJSON, run
`python3 tools/tests/test_radio_browser.py` for the real directory parsers and
SD catalog implementation. It uses temporary files, AddressSanitizer and
UndefinedBehaviorSanitizer, and injected rename/fsync failures to verify
compatibility filtering, DNS bounds, duplicate handling, persistence, limits,
and interrupted-save recovery. It requires `cc` with sanitizer support and
does not access the network or the device's SD card.

Run `python3 tools/tests/test_controls.py` to exercise the production control
decisions, preview selection, station identity, duplicate presses, key releases,
repeat timing and tick-counter rollover. It compiles against the actual BSP
input declarations and a mocked playback backend. It also checks saving an
active preview without restarting playback, and navigation with an empty catalog.

Run `python3 tools/tests/test_exit.py` to check the real F1 routing and welcome
event loop: two distinct presses, held-key rejection, cancel, display wake,
modal controls, startup exit without audio initialization, failed display retries
and the splash-before-shutdown ordering. It uses host mocks and sanitizers.

After `make build`, run `python3 tools/tests/test_ui_stack.py` to check the
ESP32-P4 compiler's actual stack frames and object-code render call sites.
Rendering must stay on the UI task (plus startup), with at most 2 KiB of
application render frames on its 8 KiB stack. This reserves space for graphics
and library calls and catches target stack exhaustion missed by host tests.

Run `python3 tools/tests/test_display_updates.py` to check redraw coalescing,
frame-rate limits, native page flips, two-frame retirement, timeout ownership,
partial updates and automatic retry delivery. These host checks do not
simulate PSRAM arbitration or establish that rare hardware underruns are gone.

Run `python3 tools/tests/test_audio_gain.py` to execute the production BSP and
codec percentage conversion functions against a register mock. It verifies the
0xB3 ceiling, monotonicity, quantized readback, bounds, and hardware errors.
`test_audio_pcm.py` checks exact mono/stereo conversion and format validation;
`test_audio_pipeline.py` checks queue retirement before rate changes, silence
until buffering completes, user mute, failed configuration and switch/stop gates.
The audio shutdown test also covers independent route levels, NVS restore,
delayed/retried saves, startup headphones, route failures and I2S restart recovery.
These checks cannot establish analog distortion or the absence of audible pops.

`test_bsp_integration.py` exercises the actual SD adapter's mount errors, retries,
idempotence and no-format policy, shared SDMMC cleanup, VCOM restoration and
native-buffer configuration. It also checks the actual HAL read implementation
for payload bounds, command/BTA/FIFO timeouts and recovery after a timeout.
It is included in `make test`, alongside the other 15 regression scripts.

`test_display_variants.py` compiles the complete production ST7701 driver and
the BSP's actual saved-calibration restoration against a panel/NVS mock. Its
31 cases cover original/v2 selection, exact ID matching, both defaults, saved
values including both defaults and register endpoints, ID/OTP errors/timeouts,
initialization failure, read-only NVS access and About labels. It counts VCOM
writes to ensure cache synchronization adds no panel write. Each case runs in a
fresh process, matching application startup. These host checks do not establish
physical display quality or replace device checks on each revision.
