import json
import pathlib
import sys


build = pathlib.Path(sys.argv[1])
config = json.loads((build / "config/sdkconfig.json").read_text())
commands = json.loads((build / "compile_commands.json").read_text())
files = {pathlib.Path(command["file"]).name for command in commands}
required = {"headless.cpp", "simulated.cpp", "parser.cpp", "state.cpp", "render.cpp", "artwork.cpp"}
if not required.issubset(files) or "smoke.c" in files:
    raise SystemExit("Incorrect headless source selection")
if config["ESP_MAIN_TASK_STACK_SIZE"] < 24576 or config.get("SPIRAM", False):
    raise SystemExit("Incorrect headless memory profile")
print("BOTTIE_HEADLESS_PROFILE_OK simulated panel128x128, no PSRAM driver")
