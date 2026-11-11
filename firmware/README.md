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
