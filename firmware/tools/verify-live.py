import json
import pathlib
import subprocess
import sys


build = pathlib.Path(sys.argv[1])
panel = sys.argv[2] == "ON"
config = json.loads((build / "config/sdkconfig.json").read_text())
commands = json.loads((build / "compile_commands.json").read_text())
files = {pathlib.Path(command["file"]).name for command in commands}
required = {"headless.cpp", "live_runtime.cpp", "client.cpp", "policy.cpp", "text_mask.cpp", "text_digest.cpp"}
if not required.issubset(files) or "smoke.c" in files or (("recovered_spi.cpp" in files) != panel):
    raise SystemExit("Incorrect live source selection")
if config["ESP_MAIN_TASK_STACK_SIZE"] != 8192 or config.get("SPIRAM", False) or not config.get("BOTTIE_NETWORK_ENABLED"):
    raise SystemExit("Incorrect live memory/network configuration")
headless = next(command for command in commands if pathlib.Path(command["file"]).name == "headless.cpp")
if "BOTTIE_LIVE_COMPANION=1" not in headless["command"] or "BOTTIE_FRAME_HEIGHT=296" not in headless["command"]:
    raise SystemExit("Live profile macros missing")
report = json.loads((build / "memory-report.json").read_text())
nm = next(line.split("=", 1)[1] for line in (build / "CMakeCache.txt").read_text().splitlines()
          if line.startswith("CMAKE_NM:FILEPATH="))
symbols = subprocess.check_output([nm, "-S", "-C", str(build / "bottie_smoke.elf")], text=True)
entries = [line.split(maxsplit=3) for line in symbols.splitlines()]
pixels = next(entry for entry in entries if len(entry) == 4 and entry[3] == "app_main::pixels")
runtime = next(entry for entry in entries if len(entry) == 4 and entry[3] == "app_main::live")
if int(pixels[1], 16) != 7680:
    raise SystemExit("Live row-band framebuffer size mismatch")
if int(runtime[1], 16) < 24576 + 2 * 18432 + 8193:
    raise SystemExit("Live runtime static buffers missing")
print(f"Static buffers: row_band={int(pixels[1], 16)}bytes live_runtime={int(runtime[1], 16)}bytes")
for region in report["layout"]:
    if region["name"] in ("DIRAM", "DRAM", "IRAM"):
        print(f"{region['name']}: linked_used={region['used']} unallocated={region['free']} total={region['total']}")
print("Tasks: main8192bytes runtime allocation, companion24576bytes static allocation; PSRAM disabled")
print("Linked totals exclude future Wi-Fi/TLS heap allocations; runtime minimum heap and stack margins unmeasured")
print("BOTTIE_LIVE_PROFILE_OK compile only, no connection or hardware verification")
