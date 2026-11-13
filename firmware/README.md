# Device firmware

The portable C++17 core parses protocol v1, validates BTAR artwork and renders RGB565 pixels. Default headless mode uses synthetic input and a simulated 128 by 128 screen. The explicit recovered/live profile targets the matched Cgotchi board at 240 by 296 with internal RAM and PSRAM disabled. The touch/brightness build received live metadata/artwork on 13 November 2026, and the user subsequently confirmed that the controls worked. Exact one-percent volume changes and restoration were not established. The new metadata-mask build requires its own commissioning.

The [recovered installed-app profile](board/recovered-profile.md) records original display provenance and matching Cgotchi documentation. That user confirmation covers the prior build broadly; script readability and resource margins on the new build remain separate checks. The default remains headless.

## Local checks

From the repository root, using the installed pinned SDK:

```fish
/usr/bin/fish --no-config firmware/tools/check-native.fish
/usr/bin/fish --no-config firmware/tools/build-headless.fish
/usr/bin/fish --no-config scripts/esp.fish smoke-build esp32s3
```

Native checks use address and undefined-behavior instrumentation. They cover malformed and oversized JSON, Unicode, duplicate keys, stale and offline state, capability restrictions, exact controls, BTAR validation, render bounds, artwork aspect fit and static face/clock output. The target helper validates the ESP image checksum, digest and target through the existing validator. The default smoke app remains selected unless `BOTTIE_HEADLESS=ON` is explicitly passed.

## Host integration harness

```fish
build/firmware/native/bottie_fixture < protocol/fixtures/playing.json
build/firmware/native/bottie_fixture --control play_pause < protocol/fixtures/playing.json
build/firmware/native/bottie_fixture --art protocol/fixtures/orbit-96.btar < protocol/fixtures/playing.json
build/firmware/native/bottie_fixture --elapsed-ms 15000 < protocol/fixtures/playing.json
build/firmware/native/bottie_fixture --wall-ms 1700000015000 < protocol/fixtures/playing.json
build/firmware/native/bottie_fixture --offline < protocol/fixtures/playing.json
build/firmware/native/bottie_fixture --frame /tmp/playing.ppm --art protocol/fixtures/orbit-96.btar < protocol/fixtures/playing.json
python3 firmware/tools/check-fixtures.py build/firmware/native/bottie_fixture protocol/fixtures
```

Buttons are `shuffle`, `previous`, `play_pause`, `next`, `repeat`. Control mode prints the POST body, never sends it. Normal mode prints effective state, progress, frame hash and whether artwork was decoded. Exit codes: 0 accepted, 1 invalid snapshot/artwork, 2 usage or I/O error, 3 control unavailable. The host owns control delivery and must refresh playback after success. Never blindly retry previous/next after ambiguous delivery.

`--frame FILE.ppm` exports the actual 128 by 128 native-rendered framebuffer as RGB pixels. These are simulated frames, never evidence of a working physical display.

Add `--profile recovered` to render and export at 240 by 296, with larger pixel text and controls. Artwork remains bounded to 240 per side independently of the framebuffer dimensions.

The core allocates no dynamic memory. JSON is limited to 8192 bytes, 256 tokens, nesting depth 8 and 64-byte keys. Track IDs are at most 128 UTF-8 bytes; title, artist, album, context name and artwork URL at most 256. Unknown fields are structurally validated and ignored within these bounds. Missing or null context/artwork is accepted. Malformed snapshots leave the prior snapshot intact and mark the model offline. JSON integers use decimal digits, with no fractional or exponent notation for protocol integer fields. Timestamps are bounded by 9007199254740991; duration by 86400000. Strings reject invalid UTF-8, unpaired surrogates and embedded NUL.

Freshness uses local monotonic time, a 15-second threshold and observation timestamp ordering. Repeated observation timestamps do not refresh the threshold. An unavailable response with the same timestamp still replaces capabilities/state. When a trusted wall clock is supplied, observation age also contributes to expiry; future timestamps beyond a five-second tolerance are rejected. Without a trusted wall clock, the first response can only be aged from receipt. Controls never update playback optimistically. Unavailable snapshots disable controls. Settings can request play from idle only when a fresh identified target explicitly permits it.

Artwork is a borrowed view of a validated complete BTAR buffer, at most 115216 bytes. Its owner must retain the bytes while rendering, associate them with the current artwork URL, clear them on URL changes and reject error responses before parsing. Rendering preserves the whole cover with aspect-fit padding. The decorative record is behind the cover. UTF-8 metadata is retained intact. ASCII and the supported Unicode punctuation keep the native lowercase pixel font. Other metadata uses the bounded text masks described below.

## Board boundary and commissioning

`board/board.hpp` separates time, playback, input, control transport and presentation. The simulated adapter logs intents without transmitting them. The live loop pauses 50 ms after each complete frame; that is not a 50 ms frame-time promise. Touch sampling is independent of full-frame duration.

The matched Cgotchi profile supplies documented display, touch and backlight wiring. Native tests establish software behavior; physical input alignment, dimming and sustained runtime require observation. Host authorization and active-player volume support remain external requirements.

## Explicit live companion mode

After importing the separately verified `network` component:

```fish
/usr/bin/fish --no-config firmware/tools/build-live.fish
/usr/bin/fish --no-config firmware/tools/build-live.fish recovered
```

The first compiles live transport with headless output; the second also opts into the recovered SPI adapter. Both use the 240 by 296 layout. Neither helper runs the application, joins Wi-Fi or flashes a device. Network configuration is blank by default and lives in the ignored component-local `network.local.hpp`, described in the network component documentation.

One background task owns the network client, JSON parsing, downloads and control delivery. The renderer only copies a bounded typed snapshot and up to 18432 artwork bytes plus 8494 text-mask bytes under a brief mutex, then releases it before applying state or rendering. It never calls transport methods. The network task holds no lock across a request. Cached artwork is copied before the transport can reuse its buffer. The renderer skips a busy handoff rather than blocking local animation.

Four button slots have a two-second queued expiry. A separate latest-desired volume slot coalesces slider motion and dispatches at most once per second. The network worker polls before dispatch, checks freshness/capabilities/target, drops invalid pending actions and never retries ambiguous POSTs. Every new physical action requires a 32-character lowercase hexadecimal control_target; the host rechecks it and pins its private playback-device ID. Old snapshots still render but cannot originate physical controls without the guard. Live headless output never generates synthetic controls.

Live stacks are 8 KiB for main, a static 24 KiB for networking and a static 4 KiB for touch. The build records linked RAM in memory-report.json. PSRAM stays disabled. Earlier firmware had bounded runtime measurements; this image needs fresh heap/stack/frame/sampling evidence.

Live mode stores sixteen rows at a time, using 7680 bytes instead of a 142080-byte full framebuffer. Each band retains the full 240 by 296 layout coordinates. One model/time/artwork/text-mask snapshot remains fixed across the entire frame; handoff consumption happens only before its first band. Raster loops clip their vertical work to the stored rows. The SPI adapter sets the matching row window for each band, including the final eight rows. Native tests reconstruct frames and compare every pixel with full-frame rendering for playing, paused, idle, offline, missing artwork, album context and clock transitions. The offline profiles and native export continue to support full-frame buffers.

The linked free-memory figure excludes runtime Wi-Fi buffers, TLS handshakes, the internal client allocation, main/system task stacks and allocator overhead. It is not a usable-heap measurement or proof of sustained networking. Commissioning must record minimum internal heap, largest free block, both task stack high-water marks, frame times and recovery through failed connections before deployment.

## Private commissioning build

`firmware/tools/build-commission.fish /tmp/bottie-commission-NAME` requires a mode 0700 directory containing a regular mode 0600 `network.local.hpp`. It includes that private header directly and keeps build output, compiler logs and the candidate manifest in the private directory. It does not retrieve credentials, activate a host listener or access USB. The caller must separately verify that the configured endpoint is authorized, reachable and authenticated. A successful compile with blank configuration is not a usable live candidate.

The commissioning profile selects the captured partition layout and 16 MiB flash geometry. It disables Wi-Fi NVS and PHY calibration persistence, flash core dumps and PSRAM. Generated bootloader/partition images are build products only: an app-only update requires checking the actual installed layout, selected slot and erase-rounded recovery range first. The manifest records source revision, dirty state, image hash and configuration hash; use a clean, independently verified source/image pair for flashing. Never apply the generated full-flash argument list to preserve an installed layout.

`BOTTIE_BOOT` reports reset cause. Every ten seconds, `BOTTIE_HEALTH` reports frames/errors/time, internal heap/main stack and current text-mask flags/bytes. `BOTTIE_NETWORK` includes poll counts, worker stack, poll duration and control success/error counts. `BOTTIE_TOUCH` reports sample/valid/error/overflow counts, maximum sample gap/read duration and brightness/settings state. These numeric lines contain no credentials or track metadata. Physical appearance and touch response must be observed separately.

## Touch and settings

Short taps act once on release. Playing icons map left to right to shuffle, previous, play/pause, next and repeat. A short tap on the nonplaying face does nothing. Hold still for 650 ms to open settings in either scene; the hold cancels any pending playback tap. Moving more than 12 pixels cancels a regular tap. Settings offers brightness, close and capability-gated resume/play. Volume appears only with fresh explicit support and a target guard. An unknown percentage is shown as volume -- without a fabricated thumb position. Closing settings preserves the original scenes.

CST810 uses address 0x15, SDA12/SCL11, 100 kHz and register0x02. Valid raw bounds are x0..295/y0..239; portrait coordinates are `(raw_y, 295-raw_x)`. A dedicated task samples every nominal20 ms with a driver wait of at least one tick (10 ms here). A32-report timestamped queue preserves press/release edges across long frames. Overflow, a200 ms input gap or obsolete queued input cancels the gesture until release. No separate reset, IRQ scan or GPIO37 access occurs. `touch_device.hpp` and `PointerQueue` are the actual native fake-bus/queue seams.

A target, freshness, playback-state, track or volume-layout change discards every report sampled through that frame boundary. A later valid release must precede a new press. This prevents queued input from being reassigned to a new player or a changed control layout, including an old release followed by another old press.

GPIO13 uses active-high25 kHz/10-bit LEDC on the documented switch. Boot remains100%; UI range10..100%. Settings are RAM-only and reset at boot; no NVS or partition change. Power-holdGPIO2 staysLOW. Shared LCD/touch reset17 is used only by existing panel startup. Physical keys, audio, motion and charge sensing remain untouched.

For simulated overlay exports add `--settings --brightness 50` to `bottie_fixture`; a capable guarded snapshot also shows volume. `bottie_input_tests` covers raw bounds/corners, a60 ms tap entirely inside a170 ms render interval, long-press suppression, stale release, target invalidation, coalescing and full/band overlay equality.

## Original-script metadata

The optional `text_mask` descriptor accepts only layout1 and `/v1/text/<64 lowercase SHA256 hex>`; malformed optional descriptors are ignored without discarding valid playback or controls. The background worker verifies the complete body digest before accepting a BTXT v1 A4 bundle. The 16-byte header and fixed packed planes are bounded to 8494 bytes. Frame geometry is240x296. Header planes use18 rows, the title26 and artist18, inside existing gaps. See `protocol/text-mask.md` in the combined checkout for the exact wire layout.

The host shapes Bengali, Japanese and mixed-script names with pinned fonts. Names stay unchanged in JSON and cache identity. Native ASCII and supported punctuation preserve the previous pixels. A missing or unsupported shaped line displays `...`; it never borrows a previous track name. English settings and all input behavior are unchanged. Other frame sizes retain the native fallback rather than scaling the fixed mask layout.

`TextCache` invalidates on any exact track/title/artist/album/context/descriptor change. Progress, timestamps and the control guard are not text identity. Digest validation protects binary integrity; metadata comparison rejects late publication for an older snapshot. `CachedPlayback` deep-copies masks into shared and displayed buffers, with no borrowed network pointer surviving the handoff. The displayed copy stays fixed through every band. Alpha compositing clips work to stored rows and retains each label color.

The companion has already bounded names to256 UTF-8 bytes before the gateway sees them. That boundary can lose a grapheme suffix. The host conservatively discards a final potentially incomplete cluster for253..256-byte values and adds an ellipsis; it cannot recover missing source bytes. Layout ellipsis is cluster-safe and reshaped on the host. No fonts, shaper, new partition or PSRAM allocation are added to firmware.

Native real-bundle export uses `bottie_fixture --profile recovered --text bundle.btxt --art cover.btar --frame output.ppm < playback.json`. The descriptor must contain the exact binary SHA256 or export rejects it. `bottie_text_tests` covers malformed/partial bundles, semantic flags, exact cache identity, late fetch rejection, ownership, alpha nibbles and1/16/31-row reconstruction including the final short band. These checks establish software behavior; physical script readability and new runtime memory margins still need observation after an authorized update.

## Selective rendering and clock

Nonplaying states use a static pixel face and a minute clock. The optional snapshot `clock` object contains integer `unix_ms` (946684800000 through4102444799999) and `utc_offset_min` (-840 through840). Invalid optional clock data leaves playback valid. Before the first valid synchronization the clock shows `--:--`; otherwise the last valid anchor advances using monotonic elapsed time even while offline. Repeated identical anchors do not stop the clock. A rejected older playback snapshot cannot reset time. The cosmetic clock never participates in control-target, gesture or text-mask identity. No RTC, NVS setting or new network endpoint is used.

`DirtyBands` compares bounded visual state before rasterization. It skips unchanged16-row bands, restricts record/progress animation to their rows, and redraws the complete playing scene when copied artwork changes because its palette also affects text and controls. Initial output, state changes, overlay open/close and transmission recovery redraw all rows. Failed transmission never marks a band clean. Text changes invalidate their old and new bounds. Frame metadata/time/artwork/text/settings remain fixed while a pass presents bands; input can interrupt a pass to show changed settings next, retaining incomplete bands. A target/state/track transition fences queued input until the visible pass finishes, then requires a fresh release before a new press.

The20ms touch sampler keeps held samples and transitions, wakes the render task, and omits redundant resting releases. The render task drains input between blocking band transfers. Idle waits up to1s for cosmetic clock/freshness checks, with input and metadata notifications waking it early. No DMA ownership, SPI frequency, CPU frequency or panel wiring changes are involved. `BOTTIE_REDRAW` reports actual bands, pixel bytes, aggregate render/present time and input age for later physical profiling.

The native `bottie_efficiency_tests` reconstructs the simulated panel through metadata/late-palette/overlay/clock/error transitions and uses the same production read-loop cancellation seam for held headers and bodies. `bottie_benchmark FIXTURE_DIR orbit-96.btar` reports seven repetitions, native render times and recorded present windows/chunks/bytes. Build it with Release `-O2 -DNDEBUG`; the same benchmark can link the pinned c72 core with `-DBOTTIE_BASELINE`. Host timings and simulated SPI counts do not establish device frame rate or power savings.
