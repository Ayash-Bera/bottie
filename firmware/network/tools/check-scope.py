import pathlib
import subprocess

root = pathlib.Path(__file__).resolve().parents[3]
base = '2f2071a17108894d1af6a76d2bda2c89688553a2'
def git(*args):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True)

paths = set(git('diff', '--name-only', base).splitlines())
paths.update(git('ls-files', '--others', '--exclude-standard').splitlines())
assert paths and all(p.startswith('firmware/network/') for p in paths), paths
subprocess.run(['git', '-C', str(root), 'diff', '--check', base], check=True)
private = 'firmware/network/network.local.hpp'
assert git('check-ignore', private).strip() == private
assert not git('ls-files', private).strip()
assert not any(p.endswith(('.bin', '.elf', '.map', '.pem', '.key', '.log')) for p in paths)
print('BOTTIE_NETWORK_SCOPE_OK')
