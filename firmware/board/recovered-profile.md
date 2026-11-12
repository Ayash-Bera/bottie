# Recovered installed-app profile

This opt-in profile uses settings traced from the installed app. Physical commissioning has shown readable text, artwork and controls in the expected orientation, with a reported blue tint. The RGB channel-order correction below still requires physical confirmation. Exact board identity, visible dimensions, power circuitry and touch hardware remain unverified.

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

The table in `recovered_profile.hpp` includes the three initial DCS commands and all 26 subsequent traced calls. The only parameter change is the color selector described below; delays and other parameters retain their traced values. Its native test checks the adjusted command-stream fingerprint. The published [JD9853 driver](https://github.com/mydazy/esp_lcd_jd9853/blob/main/esp_lcd_jd9853.c) supports the family identification; it does not identify this board.

## Renderer color order

The renderer and BTAR contract store red in bits 15..11, green in bits 10..5 and blue in bits 4..0. The adapter sends the high byte first. Actual target diagnostics confirmed that complete rendered frames and serialized SPI buffers match. A physical photo showed intact geometry with cyan-blue artwork and accents, supporting a channel-order mismatch rather than a uniform framebuffer defect.

The profile now sends `MADCTL=00` instead of the captured app's `08`. The [JD9853 datasheet](https://files.waveshare.com/wiki/common/Jd9853_datasheet.pdf#page=136), page 136, defines bit 3 as the RGB/BGR selector; page 41 defines the existing RGB565 serial bit order. Clearing bit 3 preserves orientation, geometry and byte order. The original assets' encoding is unknown, so their captured selector is not a color contract for the new renderer. Native tests cover red, green, blue, white and black byte sequences. Correct physical colors still require observation after deployment.

The adapter reproduces two additional levels from the active startup trace: GPIO2 low at `0x4200208a`, GPIO13 low at `0x42002098`, then GPIO13 high at `0x420022d5` after panel initialization. Both pins are configured as outputs by the preceding calls. GPIO13 stays low if initialization fails. Their physical roles remain unverified; GPIO13 may gate display visibility, but no backlight function is assumed. The adapter has no touch input, performs no controller readback and enables no PSRAM. No separate pin probing is implemented.

## Compile and inspect

```fish
/usr/bin/fish --no-config firmware/tools/build-recovered.fish
build/firmware/native/bottie_fixture --profile recovered --frame /tmp/recovered.ppm --art protocol/fixtures/orbit-96.btar < protocol/fixtures/playing.json
```

The compile helper selects `BOTTIE_RECOVERED_PANEL=ON`, uses a 240 by 296 internal framebuffer and validates the image. It does not access USB or flash the device. The resulting program would initialize the traced SPI/reset pins if deliberately flashed and run later. Synthetic playback remains explicit; touch and control input are disabled. The renderer fills the recovered address window rather than reproducing the old app's centered square assets.

Pixel transfers use bounded eight-row chunks. Native checks verify the complete byte stream, stride handling and chip-select cleanup on failure. Those tests establish software behavior only. Physical commissioning still requires color confirmation, panel and board markings or a schematic, GPIO13/GPIO2 role verification and touch identification.

The separate live mode uses sixteen-row render storage and updates the corresponding row window. Its final eight-row band ends at row295. This saves134400 bytes of internal memory while retaining the accepted full-frame pixels; native equality and address-window checks cover the change. Bounded runtime captures measured frame timing and memory margins; visible tearing remains unmeasured.
