# Local build setup

Run these commands from the repository root. This Linux setup uses fish without startup configuration and stores SDK files outside the repository.

## Versions

| Tool | Verified version |
| --- | --- |
| ESP-IDF | `v6.1`, commit `fff9895c82d744c7237be8847347bdd1b07c6643` |
| Python | `3.14.3`, isolated SDK and diagnostic environments |
| esptool | `5.6.0` |
| Xtensa and RISC-V GCC | `15.2.0`, toolchain release `esp-15.2.0_20251204` |
| CMake | `4.0.3` |
| Ninja | `1.12.1` |

Validation used Arch Linux x86_64. The pinned SDK accepts Python 3.10 or later; the dependency snapshots below record the tested Python 3.14 environment. Git, Python, uv, fish, make, GCC, flex, bison, gperf and pkg-config were already installed. Install missing host prerequisites for your distribution. CMake and Ninja are installed locally by the SDK tool manager. Ccache is optional and disabled by the build helper.

## Install

Use these steps for a fresh installation:

```fish
set -l bottie_base $HOME/.local/share/bottie
set -gx IDF_TOOLS_PATH $bottie_base/idf-tools-v6.1
mkdir -p $bottie_base
git clone --branch v6.1 --depth 1 --shallow-submodules --recursive https://github.com/espressif/esp-idf.git $bottie_base/esp-idf-v6.1
git -C $bottie_base/esp-idf-v6.1 rev-parse HEAD
uv venv --python /usr/bin/python3 $bottie_base/diagnostics-5.6.0
uv pip sync --python $bottie_base/diagnostics-5.6.0/bin/python requirements/diagnostics.txt
python3 $bottie_base/esp-idf-v6.1/tools/idf_tools.py install cmake ninja --targets=esp32c3,esp32s3
/usr/bin/fish --no-config $bottie_base/esp-idf-v6.1/install.fish esp32c3,esp32s3
```

Compare the downloaded commit with the pin above. The helper also checks it before activating the SDK. ESP-IDF uses release-specific package constraints. [`requirements/idf.txt`](../requirements/idf.txt) records the installed SDK packages; [`requirements/diagnostics.txt`](../requirements/diagnostics.txt) pins the separate diagnostic environment. To reproduce the SDK package snapshot with Python 3.14:

```fish
uv pip sync --python $HOME/.local/share/bottie/idf-tools-v6.1/python_env/idf6.1_py3.14_env/bin/python requirements/idf.txt
```

The helper sets `IDF_PATH` and `IDF_TOOLS_PATH` for its process and sources the SDK's `export.fish`. It does not edit shell startup files. No application credentials or environment variables are required. The SDK, compiler downloads and environments remain under `~/.local/share/bottie`.

## Build and validate

```fish
/usr/bin/fish --no-config scripts/esp.fish versions
/usr/bin/fish --no-config scripts/esp.fish smoke-build esp32c3
/usr/bin/fish --no-config scripts/esp.fish smoke-build esp32s3
```

The helper resolves the repository and firmware source from its own location. It also works from another directory when invoked using its absolute path. Source is in `firmware/`; generated files are in `build/firmware/smoke-<target>/`.

Each build produces `bottie_smoke.bin`, its ELF, bootloader, partition table and flash argument metadata. The validator checks the project target, image chip ID, checksum, SHA-256 digest, native USB console selection and disabled security provisioning settings. An intentional wrong-target validation was rejected. A device build without a recorded confirmed target is rejected before activating the SDK.

Both targets use DIO flash at 40 MHz and the SDK's default 2 MB capacity. These are smoke-build settings, not measurements of the connected board. The C3 image requires revision v0.3 or later; the S3 image requires v0.0 or later. Inspect actual hardware before adapting the configuration for deployment.

The two smoke targets validate the installed RISC-V and Xtensa compiler families. Neither binary has been run on hardware, and neither identifies the attached device or the earlier game.

Generated configuration, build output, managed components, local device records, environment files, logs and private keys are ignored by Git. SDK files are not part of the repository.

Screenshots are not applicable to this change: it contains build tooling, source organization and documentation, with no meaningful visual application behavior.

## Official references

- [ESP-IDF v6.1 release](https://github.com/espressif/esp-idf/releases/tag/v6.1)
- [Manual installation and fish activation](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/get-started/linux-macos-setup-legacy.html)
- [SDK tool installation and version selection](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-guides/tools/idf-tools.html)
- [esptool isolated installation](https://docs.espressif.com/projects/esptool/en/latest/esp32/installation.html)
- [esptool 5.6.0 release](https://github.com/espressif/esptool/releases/tag/v5.6.0)
