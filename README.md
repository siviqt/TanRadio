# TanRadio

TanRadio is an Internet radio application for the Tanmatsu platform.
This source builds **0.8.4, AppFS revision 15**, using BSP/LCD 2.0.0 for both
Tanmatsu display variants.

On every launch, an 800 × 480 welcome screen explains Wi-Fi setup and the
SD-card station catalog. Press **Space** to close it and start TanRadio.
Wi-Fi, audio, and the station catalog initialize only after Space is pressed.
After Space, the UI opens immediately with a “Starting radio...” view while
Wi-Fi transport and SD-card setup run in the background. Volume, illumination
and confirmed exit remain usable. The station screen appears when setup
finishes; saved-network attempts continue in the background. An early Play
request waits for Wi-Fi; search asks you to retry after Wi-Fi connects.
The radio is reset on entry to discard any previous launcher's SDIO state;
this initialization runs in the background.

The Midnight theme uses a navy background, slate panels, cyan selection outlines,
Saira Condensed headings and Saira Regular body text. Status and help labels are
at least 18 pixels, with larger station names and search results.

Press **F1 Options/Exit** to open TanRadio Options. Press **F1 again** to return
to the launcher, **F2** or **Esc** to restore the previous screen, or **F3** to
open Settings. Exit requires two distinct F1 presses. Options opens from the
welcome screen, startup, player, search and removal dialog. It wakes the
screen when needed and keeps volume keys available. The second F1 press
silences audio. The goodbye splash appears before the app closes the
external display and returns to the launcher.

## Settings and external display

Use **Up/Down** to select a setting and **Left/Right** to edit. **F4 Apply**
saves preferences on the device. **F2** or **Esc** discards unapplied edits
and returns to Options. From the welcome screen, external display preferences
take effect when you press **Space** to start TanRadio.

- **External display** starts Off. Select **Nokia 5110** to show the current
  station, track title, playback state and a monochrome logo on a PCD8544 LCD.
  The bottom row also shows battery level and charging, plus volume and mute.
- **Contrast** is available when Nokia is selected. The default is 60.
- **Download station logos** starts Off. Turn it on to fetch the active
  station's Radio Browser icon when it has a station UUID. Local artwork
  takes precedence and works with downloads turned Off.
- **LED brightness** defaults to 50% and adjusts the RGB status LEDs from
  0% to 100% in 5 percentage-point steps. **F4 Apply** updates the LEDs
  and saves the brightness for the next launch.

The Nokia driver uses the following Tanmatsu CATT wiring. Leave the LCD's
`LIGHT` disconnected and unplug the external JTAG adapter.

| LCD signal | RST | CE | DC | DIN | CLK | VCC | GND |
|---|---:|---:|---:|---:|---:|---:|---:|
| CATT physical pin | 8 | 5 | 7 | 10 | 9 | 1 | 2 |
| ESP32-P4 | GPIO 5 | GPIO 15 | GPIO 4 | GPIO 3 | GPIO 2 | 3.3 V | Ground |

Settings reports **Nokia initialized · connection unverified** because the
LCD interface is write-only. If an operation fails, check the wiring and
use **F4 Apply** to retry.

## Now Playing

TanRadio shows track or programme text when the stream supplies ICY titles.
Long labels scroll within the built-in Now Playing panel. On the Nokia,
long station names and titles scroll a line at a time while the logo and
playback state stay in place. The Nokia font supports printable ASCII
and shows common typographic punctuation as readable equivalents.
For other streams, the display shows the station and playback state. Logos
fall back to station initials. The station catalog can keep its existing
format.

To supply a local logo, place a PNG or baseline JPEG of at most 512 × 512 and
128 KiB in `/tanradio/logos/` on the SD card. Name it
`<station-uuid>.png` or `<station-uuid>.jpg` for a Radio Browser station.
For a manually added station, use the lowercase SHA-256 of the exact stream
URL as the filename stem. On a shell, run `printf '%s' 'YOUR_STREAM_URL' | sha256sum`
to get the stem.

## Find and add stations

Press **F2** to open **Add station**. Type a station name and press **Enter**
to search Radio Browser. Press **Tab** to switch between station name and
country search (switching clears the query). In country mode, enter a country
name such as `Bulgaria` or a two-letter code such as `BG` (case-insensitive).
Press **Enter**, then use **Up/Down** to select one of up to 20 results,
then **F4** to play a preview or **F5** to save it. **F2** or **Esc** returns to the
station list. Typing or closing the dialog cancels an outstanding search;
preview playback continues when the dialog closes. Saved results are marked
**Saved**; their Save action is disabled. Moving to another result clears the
previous confirmation message. F3 stop, volume, and illumination controls
remain available while searching.

Opening and closing search only changes the UI. Rendering runs on the UI task,
and opening the dialog does not stop, mute, or replace the current station.

## Keyboard controls

| Key | Station list | Station search |
| --- | --- | --- |
| F1 | Options / Exit | Options / Exit |
| F2 | Find stations | Back to station list |
| F3 | Stop | Stop |
| F4 | Play / Mute / Unmute | Play / Mute / Unmute |
| F5 | Play next saved station | Save selected result |
| F6 / power button | Toggle illumination | Toggle illumination |
| Hyper (between left Ctrl and Alt) | About TanRadio | About TanRadio |
| Up / Down | Move highlight | Move highlight |
| Enter | Play highlighted station; never mute | Search by name |
| Esc | Highlight current station | Back to station list |
| Backspace | Remove highlighted saved station (confirmation) | Delete previous character |

F4 shows **Play** when stopped or when another station is highlighted. For
the active station it shows **Mute** or **Unmute**, including while buffering;
it does not reconnect the stream. UUID or stream URL identifies a station,
so saving a preview or finding it again preserves its active playback state.
An unsaved preview remains selected as a temporary **Preview** row when you
leave search. It disappears when you play another station or save the preview.

On the station list, Backspace opens a removal confirmation naming the
highlighted saved station. F4 removes it; F2 cancels. Enter does not confirm.
Removing the active station keeps playback running and selects it as a
**Preview**. Preview rows cannot be removed; save the station again from search
if needed. A failed removal leaves the saved list intact and offers F4 to
retry or F2 to cancel.

Arrows, Backspace and volume repeat after 500 ms, then every 100 ms. Function
keys, Enter and Esc act once per press. A screen change cancels a previous
held-key repeat. Printable text and navigation events are handled separately
so Space and Backspace are not applied twice. Only the six physical function
keys are used; no synthesized Fn+number shortcuts are required.

Tap **Hyper** on its own to open About from the welcome screen, startup view,
player or search. **Hyper**, **Esc** or **F2** closes it and restores the previous
view. Playback continues, and volume and illumination remain available. F1
opens TanRadio Options. About shows the version and AppFS
revision compiled from `metadata.json`, the firmware build version, ESP-IDF,
BSP/LCD/ES8156 driver versions, detected display variant and panel ID, build
date/time and a short ELF build ID. An unavailable panel ID is shown as Unknown.

## Station catalog

Search currently accepts direct, non-HLS MP3 streams. Refine the name or country to find
more specific results. Radio Browser's compatibility and availability data
can be stale, so individual stations may still fail to play. Directory errors
do not replace the saved station list or interrupt an existing stream.

An SD card is required. TanRadio includes no bundled stations: a new catalog
starts empty. Press F2 to find stations and F5 to save them. Existing SD-card
catalogs remain unchanged. Saved stations are appended to
`/tanradio/stations.txt`, preserving existing records and comments. Repeated
saves of the same UUID or stream URL do not add duplicates. The catalog holds
up to 128 stations and 64 KiB; names and URLs are limited to 96 and 512 bytes.
Radio Browser UUIDs are stored in `#RADIOBROWSERUUID:` comments immediately
before their station records, keeping the existing text format readable.
Temporary and backup files protect the catalog during saves; a backup is
recovered on startup if a save was interrupted between renames.

Radio Browser requests use verified HTTPS and discover mirrors through its
DNS SRV record, with retries on another mirror. Search runs in a background
task. User-initiated playback of directory stations sends a best-effort click
notification, including when a saved station is played later. Playback does
not wait for this notification. See the [Radio Browser API](https://api.radio-browser.info/).

Search reports connection failures separately from invalid or oversized
responses. A directory connection failure can also indicate a remote service
or network outage; saved stations and playback remain available. Directory
connections retain TLS certificate verification and use a five-second socket
timeout. When discovery returns only one server, a search connection failure
gets one automatic retry after 200 ms. Cancellation still stops the retry;
playback click notifications are not retried. Diagnostics identify the mirror
and TLS error without logging queries.

## Production build

Install ESP-IDF **v6.0.2**, then run `make build`. Set `IDF_PATH` and
`IDF_TOOLS_PATH` if your SDK is installed outside the default locations shown
by `make print-sdk`. The firmware is built as `build/tanmatsu/application.bin`.

Run `make release` to generate `releases/TanRadio.bin` and its SHA-256 checksum.
Verify the generated image with
`cd releases && sha256sum -c TanRadio.bin.sha256`. This is the application image
for installation through the Tanmatsu launcher/AppFS tooling; it is not a
merged full-flash image.

The application uses **badge-bsp 2.0.0**, **LCD abstraction 2.0.0** and
**ES8156 0.1.0** in `local-components/`, retaining TanRadio's previously tested
audio, display and input fixes. The BSP configures left-justified 16-bit stereo
output and serializes codec transactions on the shared I2C bus. Startup volume
defaults to 50% independently for speaker and headphones, with 1% adjustment
steps. Connecting/disconnecting headphones restores that output's saved volume;
the status bar identifies the active output. Levels are saved to application NVS
after adjustments settle and again on normal exit. The local BSP applies the
upstream 0.70 scaling, limiting maximum codec gain to approximately -6 dB.

Playback accepts 16-bit mono/stereo MP3 output at 44.1 or 48 kHz. Mono is duplicated
to both channels, and sample-rate transitions discard queued audio before changing
I2S clocks. Unsupported formats stop with an on-screen error.
Keep the local components together; the registry BSP/driver pair is not an
equivalent replacement. See [local-components/README.md](local-components/README.md).

The BSP owns SD power and the shared SDMMC host; the application mounts through
its storage API after Space, with no automatic formatting. The local ESP-IDF
LCD HAL supplies bounded DSI reads required by the LCD abstraction, without
changing the installed SDK. The configuration explicitly enables the LCD
abstraction's vendored-HAL option so both panel ID and OTP reads are bounded.
Saved VCOM calibration is restored by the BSP.

The LCD driver reads the panel ID on each application launch. ID `42 00 01`
selects the SDK's second-display initialization sequence; other IDs use its
original sequence. Both sequences are unchanged. The local VCOM correction
only synchronizes the driver's RAM cache with the selected default (`0x2D`
for v1, `0x2E` for v2), before saved calibration restoration. It adds no panel
or persistent writes. A successfully restored saved value overrides either
default without adjustment. About reads the cached ID without another DSI read.

The owner reports that 0.7.2 revision 9 works as expected on their existing
device, where About shows `v1 (ID FF FF FF)`. This ID selects the original
display profile; it does not identify a more specific panel model. Hardware
validation on v2 remains pending. The firmware build, About information
and BadgeLink revision derive from `metadata.json`. Its `bsp_version: 2` entry
declares updated-display support to the application repository used by launcher 0.3.0.

`dependencies.lock` pins the other components. ESP-IDF downloads them into
`managed_components/` when needed; registry access is required unless they are
already cached. Generated dependencies and build outputs are not source files.

The display uses RGB565, native framebuffer switching with safe page retirement,
and a 256 KiB L2 cache with 128-byte lines. The preceding integration's device test completed
15 station starts without an observed blue flash. See [ARCHITECTURE.md](ARCHITECTURE.md)
for ownership and timing details and [CHANGELOG.md](CHANGELOG.md) for validation.

Run `make test` after a build for the regression suite. Host checks require
Python 3 and a C compiler with pthreads and sanitizer support; the UI stack check
also uses the ESP32-P4 build output. See [tools/README.md](tools/README.md).
Use `make clean` to remove build outputs, or `make fullclean` to also
remove downloaded dependencies. Both preserve the production configuration
in `sdkconfig_tanmatsu`, the defaults in `sdkconfigs/`, and any locally generated release.

## Install through BadgeLink

Return to the launcher and enable BadgeLink mode. Set `BADGELINK` to the installed
BadgeLink executable or script; it can also be a command available on `PATH`.

```sh
make install BADGELINK=/path/to/badgelink.sh
make run BADGELINK=/path/to/badgelink.sh
```

These targets upload/start the AppFS application named `tanradio`. They do not
flash the launcher or coprocessor. The Tanmatsu's main processor uses ttyACM0
when exposing its debug serial interface; ttyACM1 is the coprocessor. BadgeLink
can use its direct USB interface, so a serial device need not appear in that mode.

## Source sharing

Run `make package` to build, check and prepare separate repository and Git
source packages under `dist/TanRadio-<version>-r<revision>/`. The repository ZIP contains
`com.github.siviqt.tanradio/` with the binary, metadata, icons, license and
application README, ready to copy into the Tanmatsu app repository. The separate
source archive contains the matching application source and binary for a
personal Git repository. Packaging keeps only the archives, README and checksums;
unpacked staging copies are removed. Verify the package with
`sha256sum -c SHA256SUMS` from its output directory. Packaging refuses to overwrite
an existing destination; move that folder before recreating the same release.

After `make release`, run `make source-export` to create the source-and-release
snapshot at `dist/TanRadio-source/`. It contains the current application
source, local components and their licenses, build configuration, pinned
dependencies, assets, documentation, tests, and the current binary/checksum.
The export includes neither Git history nor generated build/download caches, machine
SDK paths, or component-publishing workflows. Export fails if the privacy
check detects private machine paths or credential material. Run
`python3 tools/check_privacy.py dist/TanRadio-source` to repeat that check.

The export includes `SOURCE_MANIFEST.sha256`. From the exported folder, run:

```sh
sha256sum -c SOURCE_MANIFEST.sha256
make build
make test
```

Install ESP-IDF v6.0.2 and set its paths as described above before building.
`make source-export SOURCE_EXPORT=/another/new/folder` selects another output
location. Export refuses to overwrite an existing folder, preserving any edits
or Git repository you have created there. `dist/` is ignored by the working
repository; the exported files are ready to add to your separate repository.
