# TanRadio 0.8.4, revision 15

`TanRadio.bin` is the Tanmatsu ESP32-P4 AppFS image built with ESP-IDF 6.0.2
and BSP/LCD 2.0.0. This release adds saved brightness for the RGB status LEDs.
Open **F1 → F3**, choose **LED brightness**, and press **F4 Apply** after editing.
Brightness defaults to 50% and changes by 5 percentage points.

Now Playing uses ICY stream metadata, scrolls long labels, and supports
local or opt-in station logos. Settings also configures the Nokia 5110
external display, which shows playback state, battery, and volume.

The firmware passed all 19 regression scripts. A BadgeLink readback matched
this image byte for byte, and the owner accepted the build after testing it on
Tanmatsu.

Run `make release` from the project root to generate the binary and checksum.
Then verify the binary from this directory:

```sh
sha256sum -c TanRadio.bin.sha256
```

Install through the Tanmatsu application repository or upload to AppFS with
BadgeLink using the slug `com.github.siviqt.tanradio` and revision **15**.
The source and app-repository archives are under `dist/TanRadio-0.8.4-r15/`
in the working checkout.
