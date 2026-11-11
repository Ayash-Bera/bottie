# Offline integration

The host lane owns companion code, the v1 protocol, shared fixtures and this harness. The firmware lane supplies its verified portable core before integration. No serial device is opened by these checks. Existing ESP smoke build scripts remain unchanged.

From the repository root, after both source commits are integrated:

```fish
fish --no-config scripts/integration.fish
```

The script runs the native firmware checks, builds two temporary Go executables, starts an authenticated fixture service on an unused loopback port and invokes `bottie_fixture` with real HTTP snapshots. It generates commands through the device's native control mapper and sends their exact JSON to the service. It checks play/pause, next/previous, shuffle, all repeat modes, playlist/album/unknown fallbacks, full BTAR transport, stale/offline controls, invalid JSON, oversized input and truncated artwork. Both 128 by 128 simulated and 240 by 296 recovered render profiles consume the shared snapshots and artwork. Success prints `INTEGRATION_OK`. Temporary server data and executables are removed; no `.env` is loaded and no Spotify or device credentials are used. The short gap between selecting and binding a free port can produce a port conflict, in which case rerun.

Run the host checks separately:

```fish
cd companion
go test ./...
go build ./...
go vet ./...
```

Firmware target compilation is a separate gate, from the repository root:

```fish
fish --no-config firmware/tools/build-headless.fish
fish --no-config firmware/tools/build-recovered.fish
fish --no-config scripts/esp.fish smoke-build esp32s3
```

The recovered profile is derived from the installed application's configuration; it has not been physically validated. A successful native test or ESP build does not verify the physical panel, touch, pins, networking or audio. Device reconnection and hardware commissioning remain separate.

Static shared fixtures live in `protocol/fixtures`. `missing.json` intentionally omits context and artwork to test tolerant clients. `invalid.json` has a negative progress value and must be rejected. `bounded.json` contains maximum-sized metadata, including UTF-8 text; `oversized-metadata.json` exceeds the title limit by one byte and must be rejected. `stale.json` has an old observation time. `orbit-96.btar` is an original deterministic cover; its payload is compared byte for byte with the running service, then decoded by the native firmware core. These files contain no account data.
