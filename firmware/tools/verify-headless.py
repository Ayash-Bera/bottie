import json
import pathlib
import sys


build = pathlib.Path(sys.argv[1])
config = json.loads((build / "config/sdkconfig.json").read_text())
commands = json.loads((build / "compile_commands.json").read_text())
files = {pathlib.Path(command["file"]).name for command in commands}
required = {"headless.cpp", "simulated.cpp", "parser.cpp", "state.cpp", "render.cpp", "artwork.cpp"}
recovered = len(sys.argv) > 2 and sys.argv[2] == "recovered"
if recovered:
    required.update({"recovered_spi.cpp", "recovered_profile.cpp"})
elif "recovered_spi.cpp" in files:
    raise SystemExit("Recovered transport must be explicitly selected")
if not required.issubset(files) or "smoke.c" in files:
    raise SystemExit("Incorrect headless source selection")
if config["ESP_MAIN_TASK_STACK_SIZE"] < 24576 or config.get("SPIRAM", False):
    raise SystemExit("Incorrect headless memory profile")
if recovered:
    headless = next(command for command in commands if pathlib.Path(command["file"]).name == "headless.cpp")
    if "BOTTIE_FRAME_HEIGHT=296" not in headless["command"] or "BOTTIE_FRAME_WIDTH=240" not in headless["command"]:
        raise SystemExit("Recovered framebuffer dimensions mismatch")
    print("BOTTIE_RECOVERED_PROFILE_OK compiled panel240x296, no physical verification")
else:
    print("BOTTIE_HEADLESS_PROFILE_OK simulated panel128x128, no PSRAM driver")
