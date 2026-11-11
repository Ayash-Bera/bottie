# Recovered installed-app profile

This profile reproduces settings traced from the installed app. It is opt-in and compiled only. The physical display, exact board, visible dimensions, power circuitry and touch hardware have not been verified. The default headless profile remains independent.

## Evidence

The exact captured image has SHA-256 `2a561070a48413278834d25d2dcececb82d417924fa77224778d339a7018f7e1`. Normal Arduino startup calls setup at `0x4200207c`, through loopTask `0x42005f6c` at call `0x42005f8d`. The startup path executes the initialization table directly, so the evidence establishes configured behavior rather than unused library support.

| Setting | Compiled evidence |
| --- | --- |
| Controller family | JD9853-compatible: vendor unlock `DF 98 53`, page changes and ordered register sequence. Fourteen parameter blocks match a published driver; ten tuning blocks differ. |
| Address window | Columns 0..239, rows 0..295 from DROM `0x3c030120` and `0x3c030124`, also present in startup window commands. |
| Pixel transfer | 142080 bytes, equal to 240 * 296 * 2. RGB565 (`COLMOD=05`) and BGR (`MADCTL=08`), without axis swap or mirroring. |
| SPI | SCK GPIO9, MOSI GPIO10, no MISO, manual CS GPIO14, DC GPIO8. Requested 40 MHz, MSB-first, mode 0. |
| Reset | GPIO17 low for 10 ms, high for 20 ms, then sleep-out and 100 ms delay. |
| Installed content layout | 240-square frames centered in the 240 by 296 transfer buffer, with 28 rows on each side. Assets were not read. |

The table in `recovered_profile.hpp` includes the three initial DCS commands and all 26 subsequent traced calls, retaining their exact parameters and delays. Its native test uses an independently computed fingerprint of the captured command stream. The published [JD9853 driver](https://github.com/mydazy/esp_lcd_jd9853/blob/main/esp_lcd_jd9853.c) supports the family identification; it does not identify this board or validate the physical panel.

The installed app drives GPIO13 high after setup and GPIO2 low. Their roles are not established. The adapter leaves both untouched. It has no touch or backlight driver, performs no controller readback and enables no PSRAM. No unknown-pin probing is implemented.

## Compile and inspect

```fish
/usr/bin/fish --no-config firmware/tools/build-recovered.fish
build/firmware/native/bottie_fixture --profile recovered --frame /tmp/recovered.ppm --art protocol/fixtures/orbit-96.btar < protocol/fixtures/playing.json
```

The compile helper selects `BOTTIE_RECOVERED_PANEL=ON`, uses a 240 by 296 internal framebuffer and validates the image. It does not access USB or flash the device. The resulting program would initialize the traced SPI/reset pins if deliberately flashed and run later. Synthetic playback remains explicit; touch and control input are disabled. The renderer fills the recovered address window rather than reproducing the old app's centered square assets.

Pixel transfers use bounded eight-row chunks. Native checks verify the complete byte stream, stride handling and chip-select cleanup on failure. Those tests establish software behavior only. Physical commissioning still requires panel and board markings or schematic confirmation, GPIO13/GPIO2 role verification, observed startup, orientation/color tests, touch identification and throughput/memory measurements before using this profile on the device.
