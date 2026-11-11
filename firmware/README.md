# Offline device slice

The portable C++17 core accepts protocol v1 playback snapshots, produces five control intents, validates BTAR artwork and renders into RGB565 memory. The ESP32-S3 profile uses synthetic playback and input. It configures no display, touch, network or PSRAM driver. Its 128 by 128 framebuffer is a simulation choice, not a measured panel specification. No binary has been flashed or run on hardware.

An optional [recovered installed-app profile](board/recovered-profile.md) supports the traced 240 by 296 address window and a separately compiled SPI adapter. This is evidence of the installed configuration, not physical validation. The default profile remains headless.

## Local checks

From the repository root, using the installed pinned SDK:

```fish
/usr/bin/fish --no-config firmware/tools/check-native.fish
/usr/bin/fish --no-config firmware/tools/build-headless.fish
/usr/bin/fish --no-config scripts/esp.fish smoke-build esp32s3
```

Native checks use address and undefined-behavior instrumentation. They cover malformed and oversized JSON, Unicode, duplicate keys, stale and offline state, capability restrictions, exact controls, BTAR validation, render bounds, artwork aspect fit and deterministic planet families. The target helper validates the ESP image checksum, digest and target through the existing validator. The default smoke app remains selected unless `BOTTIE_HEADLESS=ON` is explicitly passed.

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

`--frame FILE.ppm` exports the actual 128 by 128 native-rendered framebuffer as RGB pixels. `--seed N` selects a deterministic planet seed. These are simulated frames, never evidence of a working physical display.

Add `--profile recovered` to render and export at 240 by 296, with larger pixel text and controls. Artwork remains bounded to 240 per side independently of the framebuffer dimensions.

The core allocates no dynamic memory. JSON is limited to 8192 bytes, 256 tokens, nesting depth 8 and 64-byte keys. Track IDs are at most 128 UTF-8 bytes; title, artist, album, context name and artwork URL at most 256. Unknown fields are structurally validated and ignored within these bounds. Missing or null context/artwork is accepted. Malformed snapshots leave the prior snapshot intact and mark the model offline. JSON integers use decimal digits, with no fractional or exponent notation for protocol integer fields. Timestamps are bounded by 9007199254740991; duration by 86400000. Strings reject invalid UTF-8, unpaired surrogates and embedded NUL.

Freshness uses local monotonic time, a 15-second threshold and observation timestamp ordering. Repeated observation timestamps do not refresh the threshold. An unavailable response with the same timestamp still replaces capabilities/state. When a trusted wall clock is supplied, observation age also contributes to expiry; future timestamps beyond a five-second tolerance are rejected. Without a trusted wall clock, the first response can only be aged from receipt. Controls never update playback optimistically. Idle or unavailable snapshots disable controls.

Artwork is a borrowed view of a validated complete BTAR buffer, at most 115216 bytes. Its owner must retain the bytes while rendering, associate them with the current artwork URL, clear them on URL changes and reject error responses before parsing. Rendering preserves the whole cover with aspect-fit padding. The decorative record is behind the cover. UTF-8 metadata is retained intact; the small native font displays unsupported codepoints as a question mark. A full font and scrolling can be added after real dimensions are known.

## Board boundary and commissioning

`board/board.hpp` separates time, bounded playback delivery, input events, intent transport and frame presentation. The simulated adapter logs intents and frame hashes; it does not transmit controls. The target loop polls once per second and renders every 50 ms. It uses internal memory and a 24 KiB main task stack. The board implementation must own bounded transport buffers, artwork lifetime and button/touch mapping.

A real adapter requires verified board model, panel controller and wiring, display dimensions/orientation and touch controller/pins. Then verify PSRAM, actual startup, framebuffer memory, display throughput and input behavior. Spotify account setup and network/device authentication also remain external commissioning work. Compilation alone cannot establish any of these physical outcomes.

## Explicit live companion mode

After importing the separately verified `network` component:

```fish
/usr/bin/fish --no-config firmware/tools/build-live.fish
/usr/bin/fish --no-config firmware/tools/build-live.fish recovered
```

The first compiles live transport with headless output; the second also opts into the recovered SPI adapter. Both use the 240 by 296 layout. Neither helper runs the application, joins Wi-Fi or flashes a device. Network configuration is blank by default and lives in the ignored component-local `network.local.hpp`, described in the network component documentation.

One background task owns the network client, JSON parsing, downloads and control delivery. The renderer only copies a bounded typed snapshot and up to 18432 pixel bytes under a brief mutex, then releases it before applying state or rendering. It never calls transport methods. The network task holds no lock across a request. Cached artwork is copied before the transport can reuse its buffer. The renderer skips a busy handoff rather than blocking local animation.

There are four queued intent slots, with a 15-second expiry and no retry of ambiguous POSTs. Live mode disables simulated input completely. Physical touch remains unconfigured, so this build does not originate any controls automatically. The portable control core and host integration harness can still exercise the five intents. The runtime uses static lifetime and starts once; `stop()` requests cancellation and waits for the background owner to clean up.

The live main task has an 8 KiB stack; the background task has a statically allocated 24 KiB stack. The build writes `memory-report.json` beside its ELF and prints linked internal RAM usage. PSRAM remains disabled. Runtime heap usage, task stack margins and connection behavior have not been measured on hardware.

Live mode stores sixteen rows at a time, using 7680 bytes instead of a 142080-byte full framebuffer. Each band retains the full 240 by 296 layout coordinates. One model/time/artwork snapshot remains fixed across the entire frame; handoff consumption happens only before its first band. Raster loops clip their vertical work to the stored rows. The SPI adapter sets the matching row window for each band, including the final eight rows. Native tests reconstruct frames and compare every pixel with full-frame rendering for playing, paused, idle, offline, missing artwork, album context and all five planet families. The offline profiles and native export continue to support full-frame buffers.

The linked free-memory figure excludes runtime Wi-Fi buffers, TLS handshakes, the internal client allocation, main/system task stacks and allocator overhead. It is not a usable-heap measurement or proof of sustained networking. Commissioning must record minimum internal heap, largest free block, both task stack high-water marks, frame times and recovery through failed connections before deployment.

## Private commissioning build

`firmware/tools/build-commission.fish /tmp/bottie-commission-NAME` requires a mode 0700 directory containing a regular mode 0600 `network.local.hpp`. It includes that private header directly and keeps build output, compiler logs and the candidate manifest in the private directory. It does not retrieve credentials, activate a host listener or access USB. The caller must separately verify that the configured endpoint is authorized, reachable and authenticated. A successful compile with blank configuration is not a usable live candidate.

The commissioning profile selects the captured partition layout and 16 MiB flash geometry. It disables Wi-Fi NVS and PHY calibration persistence, flash core dumps and PSRAM. Generated bootloader/partition images are build products only: an app-only update requires checking the actual installed layout, selected slot and erase-rounded recovery range first. The manifest records source revision, dirty state, image hash and configuration hash; use a clean, independently verified source/image pair for flashing. Never apply the generated full-flash argument list to preserve an installed layout.

`BOTTIE_BOOT` reports reset cause. Every ten seconds, `BOTTIE_HEALTH` reports frame attempts, transfer errors, maximum render/transfer time in microseconds, internal free/minimum/largest heap block and main-stack free high-water bytes. `BOTTIE_NETWORK` reports the numeric `network::Status`, poll/accepted/rejected counts, worker-stack free high-water bytes and latest poll duration. These lines contain no credentials or track metadata. Counters establish runtime software activity only; actual display appearance must be observed separately.
