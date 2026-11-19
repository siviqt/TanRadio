# TanRadio 0.9.1, revision 18

`TanRadio.bin` is the Tanmatsu ESP32-P4 AppFS image built with ESP-IDF 6.0.2
and BSP/LCD 2.0.0. It uses the Tanmatsu keyboard symbols across the UI, draws
the arrows in their keycaps on the rotated display, and shows **⌫ Remove**
on the player. The orange triangle labels **Cancel**, **Back**, and **Close**.

The owner confirmed the Settings arrows, player Browse arrows, and Backspace
confirmation on the test Tanmatsu using the `0.9.0-r17` shortcut-fix candidate
(AppFS CRC32 `b1db971a`). The `0.9.1-r18` rebuild adds the release version and
revision and passed all 21 regression scripts. Its SHA-256 is
`b6a26e752999664367147276754a7852f6d4dc11d0f49417ab049be557c68497`.

From the release bundle's `releases/` directory, verify the downloaded image.

```sh
sha256sum -c TanRadio.bin.sha256
```

The Tanmatsu app package uses `com.github.siviqt.tanradio` and AppFS
revision **18**. The source and app package archives use the
`TanRadio-0.9.1-r18` release name.
