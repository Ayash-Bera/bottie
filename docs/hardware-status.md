# Hardware status

The host detected an Espressif USB Serial/JTAG interface (`303a:1001`) using the Linux `cdc_acm` driver at 12 Mbps. Access to its serial node was blocked by Linux permissions. The account lacked the device's group membership, and noninteractive sudo required authentication.

The exact chip, revision, flash capacity, board model, running application, active OTA slot, PSRAM and attached peripherals remain unverified. The earlier game has not been identified in flash, and its source is not available in this repository. Host smoke builds do not resolve those questions.

This setup has not flashed or erased the device, written eFuses, changed protection settings, actuated peripherals or issued a diagnostic reset. No device binaries or raw runtime logs are included in the repository.

## Serial access

First identify the intended device node locally. The following examples use `/dev/ttyACM0`; substitute the correct node or its stable path on your machine.

```fish
stat -c '%n %a %U:%G' /dev/ttyACM0
id -nG
```

On Arch Linux, when the node belongs to the existing `uucp` group, these administrator commands add durable account access and an immediate per-user ACL:

```fish
sudo usermod -aG uucp (id -un)
sudo setfacl -m u:(id -un):rw /dev/ttyACM0
```

Other distributions may use a different serial-access group. The ACL applies to the current node and can disappear after reconnecting. A new login picks up changed account groups. If account membership is updated but the current process still lacks the group, run a command through `sg uucp -c '...'` after confirming that membership.

## Identify without flashing

```fish
/usr/bin/fish --no-config scripts/esp.fish identify /dev/ttyACM0
```

The port is required explicitly; no unique device identifier is stored in the helper. Identification uses esptool `5.6.0`, auto-detection, the ROM loader (`--no-stub`) and `flash-id`. It resets into download mode, reads chip and flash identification, then requests a normal boot. It does not replace stored firmware or write eFuses. Avoid repeated resets without a diagnostic reason.

Keep raw logs private because application output may contain credentials. Record reviewed chip, revision and flash findings in ignored `device-findings.local.json` at repository root. Only after observing the chip, record its ESP-IDF target name as the single line in ignored `device-target.local`. Install that target's tools if it differs from the two smoke targets.

```fish
/usr/bin/fish --no-config scripts/esp.fish build
/usr/bin/fish --no-config scripts/esp.fish verify-build
```

Output goes to `build/firmware/device-<confirmed-target>/`. Confirm the flash configuration, revision and board requirements before using it on hardware. The helper has no flash action. In an activated SDK environment, flashing is a separate command:

```text
idf.py -C <absolute-firmware-directory> -B <absolute-device-build-directory> -p <serial-port> flash
```

Substitute the actual absolute source and build directories and the selected serial port. That command overwrites stored firmware and was not executed during this setup.

## Interpreting evidence

USB enumeration confirms a connected interface, not successful application boot. A firmware header can identify a stored application, but an OTA device may boot another slot. Confirm the active slot before treating a stored game image as the running game. Boot output does not establish that its display or controls work.

USB descriptor revision `1.01` is not an application firmware version, and a power descriptor is not a live current measurement.

- [Diagnostic reset modes and ROM-only access](https://docs.espressif.com/projects/esptool/en/latest/esp32/esptool/advanced-options.html)
- [Linux USB error meanings](https://docs.kernel.org/driver-api/usb/error-codes.html)
