# TanRadio production changelog

## 0.8.4 revision 15 — LED brightness

- Add a saved brightness setting for Tanmatsu's RGB status LEDs. It defaults
  to 50% and adjusts from 0% to 100% in 5 percentage-point steps.
- The firmware passed all 19 regression scripts. The BadgeLink readback
  matched the build, and the owner accepted the build after testing it on
  Tanmatsu.

## 0.8.3 revision 13 — Nokia status icons

- Show battery level and charging plus volume and mute in the Nokia status row.
  Keep the shared battery readings current while the Nokia display is active.

## 0.8.2 revision 12 — Welcome screen responsiveness

- Cache the welcome background in PSRAM during the welcome session and apply
  queued key events before drawing the latest menu state.

## 0.8.1 revision 11 — Now Playing and exit refinements

- Align the Settings heading and help text and shorten the welcome-screen notice.
- Scroll overflowing Now Playing labels on both displays. Extend the Nokia font
  to printable ASCII and approximate common UTF-8 punctuation and accents.
- Stop audio on the confirming F1 press and show the goodbye splash before
  returning to the launcher.

## 0.8.0 revision 10 — Now Playing and Nokia 5110 validation build

- Read ICY titles and station headers while preserving MP3 bytes and following
  buffered playback. Display station logos from local files or opt-in Radio
  Browser lookups.
- Add a driver interface for external displays and the native Nokia 5110
  driver for Tanmatsu CATT.
- Open Options / Exit with F1 and Settings from Options with F3. Save
  external display, contrast and logo preferences in NVS.

## 0.7.2 revision 9 — BSP/LCD 2.0.0 — 2026-11-01

- Integrate the SDK's badge-bsp 2.0.0 and MIPI DSI abstraction 2.0.0. Keep
  both upstream panel initialization sequences unchanged; ID `42 00 01`
  selects v2. Retain TanRadio's audio locking/gain, input, RGB565 native
  framebuffer retirement, error propagation, storage and bounded-read fixes.
- Explicitly enable the new vendored-LCD-HAL option for panel ID/OTP timeouts.
  Record local component versions, regenerate the dependency lockfile, and
  preserve SDK source hashes and patches. Other component versions are unchanged.
- Synchronize the VCOM RAM cache after successful initialization to the
  selected sequence's default (`0x2D`/`0x2E`), before saved calibration
  restoration. Add no hardware or persistent writes; saved absolute values
  and factory OTP behavior are unchanged.
- Show the cached display variant and ID in About, or Unknown when unavailable.
  Set application metadata to 0.7.2 revision 9 with `bsp_version: 2` for the
  application repository used by launcher 0.3.0.
- ESP32-P4 build and all 16 regression scripts pass. New coverage exercises
  31 real-driver/BSP scenarios for both selections, defaults/saved values,
  exact ID matching, read/init failures and About diagnostics. Both sequence
  tables match the SDK byte-for-byte, and all 31 panel-command/calibration
  transcripts match the unmodified SDK driver. About's v1/v2/Unknown layouts
  were rendered at 800 x 480 with the production PAX/Saira fonts. The owner
  subsequently reported expected operation on their existing device, where
  About shows `v1 (ID FF FF FF)`. Hardware validation on v2 remains pending.
- Retain one working source tree, the current Tanmatsu build and release, and
  the current distribution archives. Remove obsolete audio/BSP experiments,
  old exports, previews, caches and the superseded audio investigation notes.
  Packaging now removes its unpacked staging copies after archiving. The
  device-tested firmware image is unchanged by this cleanup.

## 0.7.1 revision 8 — production BSP promotion

- The owner confirmed all requested checks passed on 0.7.0 revision 7:
  Hyper/About, playback and output switching, display stability, SD persistence,
  and exit/relaunch. Promote the tested BSP 1.6.2, LCD abstraction 1.1.0,
  ES8156 0.1.0 and LCD HAL integration into the main production source.
- Preserve audio locking/gain limits, native framebuffer retirement, input
  fixes, VCOM restoration, bounded DSI reads and the BSP storage adapter with
  automatic formatting disabled. Restore the production welcome subtitle.
- Increment metadata to 0.7.1 revision 8 and derive firmware/About/BadgeLink
  version information from it. The new firmware build is `0.7.1-r8`.
- Include SD/VCOM/DSI checks in the 15-script production regression suite.
  Prepare separate Tanmatsu repository and personal Git source packages with
  matching binaries, metadata, licenses and checksums. Retire the duplicated
  experimental source overlay; preserve its archived tested release locally.
- The versioned production image is rebuilt from the validated integration;
  the owner's device pass refers to the preceding test image, not this rebuild.

## About screen / 0.7.0 revision 7 test build — 2026-10-23

- Bind a bare Hyper/Super tap to About on welcome, startup and active screens;
  Hyper/Esc/F2 closes it without changing search selection or playback. Keep
  volume, illumination and F1 confirmation available and prevent Space from
  starting services behind the welcome popup.
- Generate the app version and revision from the repository metadata.json;
  display firmware/ESP-IDF/BSP/LCD/ES8156 versions, build time and ELF build ID.
  Package matching test metadata, icons and license with AppFS revision 7.
- ESP32-P4 build, all 14 regression scripts and BSP integration checks pass.
  About's text fits its 800 x 480 layout using the production PAX/Saira renderer.
  Keep the validated production binary available; device testing is pending.

## BSP 1.6.2 test candidate — 2026-10-23

- Add `make bsp-test` to build and package an isolated candidate with BSP 1.6.2,
  MIPI DSI abstraction 1.1.0 and the tested ES8156 0.1.0 source. Production
  sources, configuration, dependency lock and release image remain unchanged.
- Retain TanRadio's audio locking/gain limits, native framebuffer retirement
  and input fixes; retain upstream VCOM calibration and newer BSP services.
- Use the BSP storage API without automatic formatting or duplicate SD power
  ownership. Preserve deferred mounting and the four-file limit.
- Supply a project-local ESP-IDF 6.0.2 LCD HAL compatibility patch required by
  the released abstraction's timeout helper; propagate panel initialization
  errors. Keep the installed SDK intact and record both upstream patch sets.
- ESP32-P4 build, all 13 existing regression scripts, new SD/VCOM/DSI timeout
  harnesses and privacy checks pass. Image metadata/checksum and all 217 source
  archive files are verified. Hardware validation and promotion are pending.

## Device-validated audio correction — 2026-10-21

- Backport upstream Tanmatsu BSP volume scaling (0.70); retain I2C locking and
  matched left-justified output. Maximum codec gain is approximately -6 dB.
- Remember independent speaker/headphone levels in NVS, switch levels with the
  jack, and label the active output in the status bar. Defaults are 50% each.
- Disable amplifier forcing, read jack state on startup and reconcile every
  250 ms. Preserve mute and fail silent when route application fails.
- Validate PCM before queuing, duplicate mono into stereo, and coordinate
  44.1/48 kHz I2S changes with queue retirement and DMA silence. Other formats
  stop with a visible error. Recover from failed I2S enable on a later play.
- Add real BSP/driver gain, PCM and pipeline regressions; expand route,
  persistence, failure and shutdown coverage. Target build and all 13 regression
  scripts pass. Installation was read back and verified; ESP32-P4 startup
  detected the ES8156 and reached the welcome screen without an observed panic.
- Owner confirmed all requested device checks worked as expected: speaker
  playback above 75%, headphone insertion/removal with independent volume
  restoration, and checks for distortion, clicks and unexpected unmuting.
  This is qualitative device validation; instrumented distortion measurements,
  cross-relaunch persistence and the full PCM format matrix were not separately
  reported. Promote the exact tested image without rebuilding; retain the
  pre-audio image in `releases/audio-test/` for rollback.

## Country search and source privacy — 2026-10-20

- Add country search: Tab switches name/country mode, with country names and
  case-insensitive two-letter codes. Switching cancels pending results.
- Privacy audit of distributable source and firmware found no personal
  development data or credentials requiring removal. Retain public upstream
  attribution and certificate contacts. Gate source export on private-path and
  credential checks; refresh the prepared source snapshot and release.
- Build, ten regression checks and privacy checks pass. Country search awaits
  device validation.

## Previously device-validated production — 2026-10-20

- Midnight UI with readable Saira fonts, navy/slate panels and cyan selection.
- Two distinct F1 presses confirm exit; Esc/F2 cancels. The exit splash returns
  to the launcher after audio cleanup and omits “See you next time”.
- Responsive startup: UI/input open before background transport and SD setup.
  Reset the radio on each launch to discard stale SDIO state and prevent the
  oversized DMA allocation seen during warm application transitions.
- Start at 50% volume and adjust in 1% steps. Preserve responsive controls and
  safe, serialized audio shutdown.
- Search Radio Browser by station name, preview results, save stations to SD,
  and remove saved stations through confirmation. Keep active playback as a
  Preview when its saved row is removed. Preserve catalogs through failures
  and interrupted saves.
- Use verified HTTPS, mirror discovery, cancellation, bounded responses and
  one automatic retry for a connection failure when only one mirror is found.
  Intermittent remote directory TLS failures can still need a manual retry.
- Present complete native framebuffers, wait for scanout retirement before
  reuse, and retry failed updates without overwriting pending pages. Keep
  RGB565 and a ten-frame-per-second redraw limit. Use a 256 KiB L2 cache with
  128-byte lines following Espressif's DSI bandwidth guidance.
- Device validation: 15 successful station starts, responsive playback, no
  observed blue flash and correct exit text. No display/allocation/crash errors
  appeared in the final capture. Rare behavior beyond this test is unproven;
  the precise hardware cause has not been established.
- All nine regression checks and the ESP32-P4 production build pass.

An earlier device-validated application had CRC32 `c082cc59`. The current
packaged application is 0.7.2 revision 9; use its SHA-256 sidecar in `releases/`
to verify it. Rebuilding may change image metadata and
checksums. No temporary white-fill diagnostic is included.

## Project maintenance

- Keep current production source, local component patches and licenses,
  configuration, assets, documentation, regression checks and one release image.
- Remove build/download/Python caches, an unused logo source image and upstream
  component-publishing automation. Preserve the existing repository history.
- Simplify build helpers, correct BadgeLink installation to the `tanradio` slug,
  and add `make test` and `make source-export` for a clean sharing snapshot.
