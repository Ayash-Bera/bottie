import json
import pathlib
import subprocess
import sys

from esptool.bin_image import LoadFirmwareImage
from esptool.targets import CHIP_DEFS


def require(condition, message):
    if not condition:
        raise SystemExit(message)


build = pathlib.Path(sys.argv[1])
target = sys.argv[2]
description = json.loads((build / "project_description.json").read_text())
config = json.loads((build / "config/sdkconfig.json").read_text())
flash = json.loads((build / "flasher_args.json").read_text())

require(description["target"] == target, "Project target mismatch")
require(config["IDF_TARGET"] == target, "Configuration target mismatch")
require(flash["extra_esptool_args"]["chip"] == target, "Flash metadata target mismatch")
require(config["ESP_CONSOLE_USB_SERIAL_JTAG"], "Native USB console is required")
require(config["ESP_CONSOLE_SECONDARY_NONE"], "Secondary console must be disabled")
require(not config.get("SECURE_BOOT", False), "Smoke build must not provision secure boot")
require(not config.get("SECURE_FLASH_ENC_ENABLED", False), "Smoke build must not provision flash encryption")

image = build / "bottie_smoke.bin"
require(image.stat().st_size > 0, "Firmware image is empty")
require((build / "bottie_smoke.elf").stat().st_size > 0, "ELF is empty")
firmware = LoadFirmwareImage(target, str(image))
require(firmware.chip_id == CHIP_DEFS[target].IMAGE_CHIP_ID, "Image header target mismatch")
firmware.verify()
require(firmware.checksum == firmware.calculate_checksum(), "Image checksum mismatch")
require(firmware.append_digest, "Image digest is missing")
require(firmware.stored_digest == firmware.calc_digest, "Image digest mismatch")
subprocess.run([sys.executable, "-m", "esptool", "--chip", target, "image-info", str(image)], check=True)
print(f"BUILD_IMAGE_OK target={target} bytes={image.stat().st_size}")
