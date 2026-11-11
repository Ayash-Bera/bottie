import json
import pathlib
import subprocess
import sys


binary = pathlib.Path(sys.argv[1]).resolve()
fixtures = pathlib.Path(sys.argv[2]).resolve()


def run(name, *args, expected=0):
    result = subprocess.run(
        [str(binary), *args], input=(fixtures / name).read_bytes(),
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5,
    )
    if result.returncode != expected:
        raise SystemExit(f"{name}: exit {result.returncode}, expected {expected}: {result.stderr.decode()}")
    return json.loads(result.stdout) if result.stdout else None


for filename in ("playing.json", "paused.json", "idle.json", "unavailable.json", "missing.json", "stale.json"):
    result = run(filename)
    if result["state"] not in ("playing", "paused", "idle", "unavailable"):
        raise SystemExit(f"{filename}: unexpected state")
run("invalid.json", expected=1)
expected_controls = {
    "shuffle": {"action": "shuffle", "value": True},
    "previous": {"action": "previous"},
    "play_pause": {"action": "pause"},
    "next": {"action": "next"},
    "repeat": {"action": "repeat", "value": "context"},
}
for button, expected in expected_controls.items():
    if run("playing.json", "--control", button) != expected:
        raise SystemExit(f"Incorrect {button} intent")
if run("paused.json", "--control", "play_pause") != {"action": "play"}:
    raise SystemExit("Incorrect resume intent")
for filename in ("idle.json", "unavailable.json"):
    run(filename, "--control", "next", expected=3)
for args in (("--elapsed-ms", "15000"), ("--offline",), ("--wall-ms", "1700000015000")):
    if run("playing.json", *args)["state"] != "unavailable":
        raise SystemExit("Freshness failure")
    run("playing.json", *args, "--control", "next", expected=3)
art = run("playing.json", "--art", str(fixtures / "orbit-96.btar"))
if art["artwork"] is not True:
    raise SystemExit("Artwork decode failure")
print("BOTTIE_HOST_FIXTURES_OK")
