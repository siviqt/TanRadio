# TanRadio architecture

This describes the current production implementation for Tanmatsu (ESP32-P4)
and ESP-IDF v6.0.2. The launcher installs and selects the AppFS application;
TanRadio initializes its BSP services and owns radio playback and its UI.

## Startup and shutdown

`main/main.c` initializes NVS and the BSP, then calls `app_init()`.
The welcome screen runs before networking and audio start. It caches its
background in PSRAM and processes queued keys in order before drawing the
latest menu state. After Space, it releases the cache and starts audio
controls, UI and input immediately. A background task prepares network
transport and the SD-card catalog, then atomically publishes service readiness.
The UI gates catalog access and playback actions until that publication.

Transport initialization precedes SD-card setup because ESP-Hosted and the
card share the SDMMC host. Saved-network attempts run separately and do not
hold up the station screen. An early Play request waits for connectivity.
The BSP owns SD power and host setup; `storage/sdcard.c` uses its mount-state
query and advanced mount API with four open files and automatic formatting
disabled. The BSP handles shared-host cleanup on mount errors.
The SDK resets the radio on every host boot through the BSP callback to discard
stale SDIO counters from a preceding launcher/application session.

F1 opens Options and Esc or F2 cancels. A second distinct F1 press stops
audio and commits exit. The UI waits at least one second after a successful
goodbye splash submission before requesting the launcher return. Early exit
during background initialization uses the startup-safe shutdown path.
Welcome-screen exit starts neither audio nor Wi-Fi.

## Modules and ownership

| Module | Responsibility |
| --- | --- |
| `app` | Startup sequencing and background service initialization |
| `audio` | BSP audio output, volume, mute and serialized shutdown |
| `input` | Keyboard routing, distinct presses and bounded key repeat |
| `network` | Transport preparation, readiness and saved-network connection task |
| `radio` | Station catalog, stream transport, compressed/PCM buffers and decoding |
| `radio/browser` | Asynchronous Radio Browser discovery, search and click requests |
| `storage` | SD-card mounting and shared-host constraints |
| `ui` | Rendering, selection, search/removal/exit state and illumination |
| `util` | Shared application utilities |

Playback owns a copied station record. It does not retain pointers into mutable
search results or the saved catalog. Audio control remains responsive during a
blocked PCM write; shutdown rejects new PCM/unmute work before disabling output.
Speaker and headphone levels default independently to 50%, with 1% steps and
saturation at 0/100%. The audio control mutex protects route, levels, mute and
hardware changes; the UI reads one consistent route/level snapshot. Jack events
and a 250 ms reconciliation worker use the same route transition, which silences
the codec, disables speaker forcing, restores the destination level and enables
the speaker only when appropriate. Unknown jack state or route failures silence
output and are retried. Changing routes preserves playback mute.

The same worker saves `tanradio_audio` NVS keys `speaker` and `headphones` after
1.5 seconds without an adjustment. Flash writes run outside the audio control
lock, under a separate settings mutex; generation tracking preserves concurrent
changes. Normal shutdown flushes pending levels. Missing/invalid settings retain
50%. The BSP scales public volume by 0.70 and reverses scaling on readback,
retaining shared-bus serialization and a maximum codec register value of 0xB3.

The decoder validates every PCM frame's format before queuing it. Supported
formats are signed 16-bit mono/stereo at 44.1/48 kHz; mono samples are duplicated.
A pipeline mutex serializes output consumption, queue resets and format changes.
Transitions mute and discard old queued samples, clock two DMA rings of silence,
and disable/reconfigure/re-enable I2S when its rate changes. Output stays muted
until the new queue reaches its high watermark, respecting user mute. Unsupported
formats or configuration failures stop playback with a UI error. A subsequent
play request retries, including recovery from a failed I2S enable.

## Station discovery and persistence

Radio Browser runs on its own worker with verified HTTPS, bounded responses and
JSON depth, DNS SRV mirror discovery and cancellation. Failed directory requests
do not interrupt playback or replace the saved catalog. Search connection errors
retry another mirror; with only one mirror they retry once after 200 ms. Socket
operations allow five seconds. Click notifications are best effort and never
block playback. Remote TLS resets can still require a later manual search retry.

The SD catalog is `/tanradio/stations.txt`. New catalogs are empty. Saves preserve
existing records/comments and prevent UUID/URL duplicates. A temporary file and
backup protect replacements and support interrupted-save recovery. Removal
commits the file before changing the in-memory catalog. Removing the active
station preserves playback as a selectable Preview. Catalog compaction and UI
readers are serialized; indices are rebased without reconnecting the stream.

## Now Playing and external displays

The HTTP download task strips ICY framing before forwarding MP3 bytes.
A bounded queue associates each changed title with its compressed byte
position. The decoder maps that position to the next corresponding PCM
frame. The output task publishes the title after that PCM reaches I2S.
The station mutex protects the combined playback snapshot and connection
generation. Switch, stop and reconnect invalidate metadata from the
previous connection.

Artwork uses a separate worker. It checks the SD override by station UUID
and URL hash, then optionally looks up the active station's Radio Browser
favicon over verified HTTPS. Bounded image input and decode sizes produce
one RGB565 thumbnail and one monochrome thumbnail. A three-entry RAM cache
and request sequence keep network and decoding work outside the UI and
audio paths. The built-in and external displays share the active snapshot.

The UI submits a copy of the Now Playing card to the external display
worker. It retains the newest card while the selected driver owns all GPIO
or bus operations. Drivers implement open, present and close and can ask for
a timed redraw of the current card. The Nokia 5110 driver uses that timer to
scroll overflowing text a line at a time. The built-in panel scrolls long
labels on its regular Now Playing update cycle. Settings saves the driver,
contrast and logo preference together in NVS. The confirming exit key stops
audio immediately. After the goodbye splash, the UI closes the external
driver and hands off to the launcher.

## Rendering and display

After startup, rendering belongs to the UI task. Input and playback tasks request
redraws through task notifications. The UI coalesces requests to at most ten
frames per second; hardware status polling remains on a one-second cadence.
Renderer-owned station snapshots keep large records out of nested stack frames.
Target compiler stack accounting and object-code call sites are checked by
`tools/tests/test_ui_stack.py`.

PAX renders RGB565 into its application buffer. The local Tanmatsu BSP copies the
complete image into the inactive native panel framebuffer, then submits that
page to ESP-IDF's frame-boundary switching path. Two refresh completions retire
the previous page before reuse. Timeouts preserve pending ownership and request
a rate-limited retry. This avoids copying an external snapshot into the page
currently being scanned. The source copy is synchronous, so PAX may repaint
after the BSP returns.

The production configuration uses a 256 KiB L2 cache with 128-byte cache lines.
The preceding BSP 1.6.2 station-switch sequence completed 15 station starts without an
observed flash. This is device validation of the current mitigation, not proof
of a precise hardware cause or of long-term elimination of every rare flash.
No diagnostic fill-color override remains in production.

## Dependencies and distribution

`local-components/` contains badge-bsp 2.0.0, LCD abstraction 2.0.0, ES8156 0.1.0
and the ESP-IDF 6.0.2 LCD HAL compatibility component. It retains upstream
licenses, component structure and patch records. BSP initialization restores
saved VCOM calibration and propagates panel probe errors. A 100 ms shared
polling budget bounds each DSI read; callers must serialize those reads. The
current application reads panel ID and OTP during startup before its tasks run.
`CONFIG_MIPI_DSI_ABSTRACTION_USE_VENDORED_ESP_HAL_LCD=y` explicitly retains
timeout detection in both the active configuration and reproducible defaults.
The LCD driver selects the upstream v2 sequence only for ID `42 00 01`,
otherwise retaining the upstream original sequence. Initialization synchronizes
the runtime VCOM cache to that sequence's default before the BSP restores any
saved calibration. This synchronization adds no panel or persistent writes;
saved values remain absolute register settings. The factory OTP value is separate
and the BSP's existing OTP prerequisite for saved restoration is unchanged.
About reads cached ID/variant information and reports Unknown if the ID is
unavailable. The owner reports expected operation of the 2.0.0 integration on
their existing v1 device; hardware validation on v2 remains pending.
Do not replace these components with
registry copies or edit generated copies in `managed_components/`.
`dependencies.lock` pins downloaded components; ESP-IDF regenerates their cache.

`sdkconfig_tanmatsu` is the production configuration; `sdkconfigs/` supplies
reproducible defaults. Keep both. `make build` writes to `build/tanmatsu` and
ESP-IDF populates `managed_components/`. `make release` generates the AppFS
image `releases/TanRadio.bin` and its SHA-256 sidecar. `make package` writes the
distribution archives, README and checksums under `dist/`.
The sharing export includes source, local components, tests, configuration,
assets, documentation and the current application image, without Git history
or historical test payloads. `metadata.json` supplies the version/revision for
the firmware description, About screen, packaging and BadgeLink installation.
