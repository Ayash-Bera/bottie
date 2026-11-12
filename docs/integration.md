# Offline integration

The host lane owns companion code, the v1 protocol, shared fixtures and this harness. The firmware lane supplies its verified portable core before integration. No serial device is opened by these checks. Existing ESP smoke build scripts remain unchanged.

Use Go 1.26 or later, CMake and a C++17 compiler. Target builds also require the pinned ESP-IDF 6.1 installation described in [setup](setup.md). From the repository root:

```fish
fish --no-config scripts/integration.fish
```

The script runs the native core, panel and network policy checks with address/undefined-behavior sanitizers, builds two temporary Go executables, starts an authenticated fixture service on an unused loopback port and invokes `bottie_fixture` with real HTTP snapshots. It generates commands through the device's native control mapper and sends their exact JSON to the service. It checks play/pause, next/previous, shuffle, all repeat modes, playlist/album/unknown fallbacks, full BTAR transport, stale/offline controls, invalid JSON, oversized input and truncated artwork. Both 128 by 128 simulated and 240 by 296 recovered render profiles consume the shared snapshots and artwork. Success prints `INTEGRATION_OK`. Temporary server data and executables are removed; no `.env` is loaded and no Spotify or device credentials are used. The short gap between selecting and binding a free port can produce a port conflict, in which case rerun.

The network policy tests use fragmented in-memory HTTP streams to check framing, response limits, exact-origin artwork, bearer syntax, control encoding and bounded backoff. ESP Wi-Fi/TLS code is compiled separately; these native checks do not connect the device to the host. The network worker's `check-scope.py` compares its original branch base and is intended for that isolated branch, not this combined integration tree.

Run the host checks separately:

```fish
cd companion
go test ./...
go build ./...
go vet ./...
```

Language-mask tests additionally exercise pinned local font coverage, Bengali conjunct boundaries, NFC-equivalent Bengali/Japanese strings, ZWNJ preservation, mixed-script lowercase, safe ellipsis, fixed ink bounds, cache expiry/identity, response hashes and gateway authentication. Run affected races with `go test -race ./internal/textmask ./internal/devicebridge`. These checks use synthetic names and local fake upstreams, with no Spotify login or controls.

Generate deterministic names-only fixtures with installed pinned fonts:

```fish
cd companion
go run ./cmd/text-fixtures /tmp/bottie-text-fixtures
```

The five JSON/BTXT pairs cover Bengali, Japanese, mixed Latin text, album-only fallback and long labels. Shared copies live under `protocol/fixtures/text-*`. The exact deployed pre-language parser must accept these additive JSON snapshots before gateway activation. Native rendering and full-band equality establish software output only; physical language readability requires the actual panel after an independently validated configured image. See the exact [binary layout and font provenance](../protocol/text-mask.md).

Firmware target compilation is a separate gate, from the repository root:

```fish
fish --no-config firmware/tools/build-headless.fish
fish --no-config firmware/tools/build-recovered.fish
fish --no-config firmware/tools/build-live.fish
fish --no-config firmware/tools/build-live.fish recovered
fish --no-config scripts/esp.fish smoke-build esp32s3
```

The live profiles compile the actual Wi-Fi/HTTP client and background handoff task. Both use the recovered 240 by 296 layout; only the `recovered` variant includes the SPI adapter. Their verifiers check source selection, disabled PSRAM, task stack settings and the 7680-byte row buffer, and write `memory-report.json` in `build/firmware/live-{headless,recovered}-esp32s3`. Native tests reconstruct every frame from bands and compare it pixel for pixel with full-frame rendering, including fallback states and the final short band. These commands do not run, flash or connect a device.

Start the account-free companion separately:

```fish
cd companion
env BOTTIE_MODE=fixture BOTTIE_BIND=127.0.0.1:8787 go run ./cmd/server
```

Open `http://127.0.0.1:8787/` for status. For a future device connection, follow the hidden device-token prompt and LAN environment settings in [companion configuration](companion.md#configuration). Use a matching token of at least 32 characters from the firmware's supported character set, keep artwork at 96 pixels and configure an IPv4 literal origin. Create the ignored private file from the repository root without replacing existing contents:

```fish
umask 077
touch firmware/network/network.local.hpp
chmod 600 firmware/network/network.local.hpp
```

Use a local editor to add `local_config()` as shown in [network configuration](../firmware/network/README.md#private-configuration), with private Wi-Fi settings, companion origin and the same device token. Plain HTTP needs explicit `allow_insecure_http=true` on a trusted private LAN; the companion's built-in server does not provide TLS. Keep configured images private because they contain these credentials. Default builds have blank network configuration and cannot join Wi-Fi. Spotify mode additionally needs a registered client ID, exact loopback redirect URI and browser consent as described in [Spotify connection](companion.md#spotify-connection); tokens stay in host memory.

The recovered profile is derived from the installed application's configuration; it has not been physically validated. Touch input is unconfigured and live mode never generates synthetic controls. Device reconnection, physical panel identity/orientation, wiring, input mapping, PSRAM operation and actual Spotify/Alexa behavior remain unverified. On-device commissioning must measure minimum internal heap, largest free block, task stack margins, frame times and connection recovery. Static linked memory excludes subsequent Wi-Fi/TLS allocations. A successful native test or ESP build establishes neither physical behavior nor sustained networking.

Static shared fixtures live in `protocol/fixtures`. `missing.json` intentionally omits context and artwork to test tolerant clients. `invalid.json` has a negative progress value and must be rejected. `bounded.json` contains maximum-sized metadata, including UTF-8 text; `oversized-metadata.json` exceeds the title limit by one byte and must be rejected. `stale.json` has an old observation time. `orbit-96.btar` is an original deterministic cover; its payload is compared byte for byte with the running service, then decoded by the native firmware core. These files contain no account data.
