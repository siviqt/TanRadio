# TanRadio 0.9.0, revision 17

The release bundle contains the Tanmatsu ESP32-P4 AppFS image as
`TanRadio.bin`, built with ESP-IDF 6.0.2 and BSP/LCD 2.0.0. It adds audio
formats including AAC/AAC+, Ogg Opus/Vorbis, FLAC, and audio-only HLS.
**F1 → F3** opens Settings, where you can choose Vivid and save it with
**F4 Apply**. Radio Browser search loads pages in either direction and marks
the first, last, or only result in the footer.

All 20 regression scripts passed. The owner tested the audio formats, theme,
and search on Tanmatsu. The release image matched the BadgeLink readback byte
for byte. Its SHA-256 is
`cb6355f6563b11fabeeec0bf986ffc4a822521fa665646647b485fda976c7bbb`.

From the release bundle's `releases/` directory, verify the downloaded image.

```sh
sha256sum -c TanRadio.bin.sha256
```

The Tanmatsu app package uses `com.github.siviqt.tanradio` and AppFS
revision **17**. The source and app package archives use the
`TanRadio-0.9.0-r17` release name.
