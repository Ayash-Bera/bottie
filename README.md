# Bottie

ESP-IDF build tools and a native USB console smoke app. The app reports chip, SDK and memory information, then returns to idle. It has no board pin mappings, radio setup, storage writes or reboot loop.

The earlier game source is not present in this repository. The connected device's chip and installed application remain unverified. No firmware has been flashed by this setup.

## Build

Complete the [user-local setup](docs/setup.md), then run from the repository root:

```fish
/usr/bin/fish --no-config scripts/esp.fish versions
/usr/bin/fish --no-config scripts/esp.fish smoke-build esp32c3
/usr/bin/fish --no-config scripts/esp.fish smoke-build esp32s3
```

These are host toolchain checks for two explicit targets, not identification of the connected device. Output goes to `build/firmware/smoke-<target>/bottie_smoke.bin`. Each target has its own ignored configuration and build directory.

## Layout

- [`firmware/`](firmware/): smoke app, CMake project and configuration defaults.
- [`scripts/`](scripts/): fish commands and firmware image validation.
- [`requirements/`](requirements/): exact installed Python dependency pins.
- [`docs/setup.md`](docs/setup.md): SDK installation, versions and build verification.
- [`docs/hardware-status.md`](docs/hardware-status.md): observed hardware limits and diagnostic instructions.

The SDK and compilers live under `~/.local/share/bottie`, outside the repository. Device-specific records and generated files stay ignored. Building and flashing are separate operations; the helper has no flash command.
