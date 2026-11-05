# Vendored components

TanRadio 0.7.2 revision 9 integrates BSP and LCD abstraction 2.0.0 from the
supplied SDK snapshots. It retains the audio, framebuffer, input and storage
integration from the previously device-tested BSP 1.6.2 release. The owner
reports expected operation on their existing v1 device; hardware validation
of this upgrade on v2 remains pending.

`main/idf_component.yml` pins and overrides BSP/LCD 2.0.0 and the local ES8156.
Root CMake selects the local LCD HAL and Nokia 5110 component through
`EXTRA_COMPONENT_DIRS`. The SDK source manifests omit release versions,
so the vendored BSP and LCD manifests explicitly declare `version: 2.0.0` for
dependency resolution and the About screen.

| Component | Upstream base | Source provenance |
| --- | --- | --- |
| badge-bsp | 2.0.0 | SDK snapshot `esp32-component-badge-bsp-2.0.0` |
| mipi_dsi_abstraction | 2.0.0 | SDK snapshot `esp32-component-mipi-dsi-abstraction-2.0.0` |
| ES8156 | 0.1.0 | Commit `97fc872701c5f548cf94c9c4896505aa752c0260` |
| ESP-IDF LCD HAL | 6.0.2 | Commit `7101770dc6db2667b3c477cc31365dd1acd6db4e` |
| Nokia 5110 | 1.0.0 | SDK archive `nokia5110-module-1.0.0.zip`, with printable ASCII glyphs added |

The `patches/*.upstream.sha256` files record each SDK snapshot's unmodified
files, excluding Git metadata and GitHub workflows. Their own SHA-256 values are:

- `badge-bsp-2.0.0.upstream.sha256`:
  `961b5d37977d1b4b39b739388d137d8c08bd4b774cf55e5758f129d4e68b146b`.
- `mipi-dsi-abstraction-2.0.0.upstream.sha256`:
  `93ff5b32d9e0d71992eb02def1ce7f88d07607d7f7a51d0407904d4c9b830e25`.

## Display variants and VCOM

The LCD driver reads the panel ID at startup. ID `42 00 01` selects the second
panel's sequence; other IDs retain the original sequence. Both upstream
initialization sequences are unchanged. About uses the cached ID and selected
variant, with Unknown when the ID is unavailable; it issues no additional read.

After successful initialization, the local correction synchronizes the VCOM
RAM cache to the sequence's value: `0x2D` for v1 or `0x2E` for v2. It adds no
panel commands, flash writes or factory OTP changes. The BSP subsequently
restores `system/lcd_vcom` through the existing setter, updating both hardware
and cache. Saved settings are absolute register values, not offsets; their
restoration conditions, including the factory OTP read prerequisite, remain
unchanged. The cache is never reset to the default after saved restoration.

## Retained TanRadio changes

- Audio: shared I2C locking, finite/clamped volume, upstream 0.70 gain scaling
  and matched left-justified 16-bit stereo I2S. The maximum codec register is
  0xB3 (approximately -6 dB). ES8156 C/header sources remain upstream 0.1.0.
  The app owns independent saved speaker/headphone volumes and safe shutdown.
- Display: two native framebuffers, frame-boundary switching, two-refresh page
  retirement and timeout ownership. Retain upstream VCOM APIs and saved
  restoration; declare `nvs_flash` and propagate panel initialization errors.
- Input: upstream right-Shift reporting/common hooks plus corrected dormant
  repeat bookkeeping. The app remains the only active repeat engine.
- Storage: the app delegates to BSP storage, defers mounting until Space,
  allows four open files and never auto-formats. The BSP owns power and the
  shared SDMMC/ESP-Hosted cleanup workaround.
- LCD HAL: the local copy bounds command/BTA/FIFO polling with one
  100,000-microsecond budget, clears the receive buffer and exposes the timeout
  helper. `CONFIG_MIPI_DSI_ABSTRACTION_USE_VENDORED_ESP_HAL_LCD=y` explicitly
  enables timeout checks for both panel ID and OTP probes. Reads are serialized
  during startup, before application tasks run.

`patches/badge-bsp-2.0.0.patch`, `patches/mipi-dsi-abstraction-2.0.0.patch` and
`patches/esp-hal-lcd-6.0.2.patch` record changes against their upstream sources.
Keep all components together and retain their licenses/manifests. BSP/LCD/ES8156
are MIT; the LCD HAL is Apache-2.0. Do not edit generated component copies in
`managed_components/`.
