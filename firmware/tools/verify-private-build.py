import hashlib
import json
import pathlib
import stat
import subprocess
import sys


private = pathlib.Path(sys.argv[1]).resolve()
if private.parent != pathlib.Path('/tmp') or not private.name.startswith('bottie-commission-'):
    raise SystemExit('Commissioning directory must be /tmp/bottie-commission-*')
if not private.is_dir() or stat.S_IMODE(private.stat().st_mode) != 0o700:
    raise SystemExit('Commissioning directory must have mode0700')
header = private / 'network.local.hpp'
if header.is_symlink() or not header.is_file() or stat.S_IMODE(header.stat().st_mode) != 0o600:
    raise SystemExit('Private network.local.hpp must be a regular mode0600 file')
local_header = pathlib.Path(__file__).resolve().parents[1] / 'network/network.local.hpp'
if local_header.exists():
    raise SystemExit('Remove the workspace-local configuration to avoid ambiguous precedence')
if '--image' in sys.argv:
    build = private / 'build'
    config = json.loads((build / 'config/sdkconfig.json').read_text())
    if config.get('ESPTOOLPY_FLASHSIZE') != '16MB' or config.get('PARTITION_TABLE_OFFSET') != 0x8000:
        raise SystemExit('Unexpected commissioning flash geometry')
    if any(config.get(key) for key in ('ESP_PHY_CALIBRATION_AND_DATA_STORAGE', 'ESP_WIFI_NVS_ENABLED',
                                      'ESP_COREDUMP_ENABLE_TO_FLASH', 'SECURE_BOOT', 'SECURE_FLASH_ENC_ENABLED',
                                      'BOOTLOADER_APP_ANTI_ROLLBACK', 'SPIRAM')):
        raise SystemExit('Unexpected persistent storage, security or PSRAM configuration')
    image = build / 'bottie_smoke.bin'
    data = image.read_bytes()
    aligned = (len(data) + 4095) & ~4095
    if not 0 < aligned <= 0x140000 or stat.S_IMODE(image.stat().st_mode) & 0o077:
        raise SystemExit('Image exceeds selected slot or lacks private permissions')
    source = pathlib.Path(__file__).resolve().parents[2]
    commit = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    dirty = bool(subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain', '--', 'firmware'], text=True))
    manifest = {'image': str(image), 'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data),
                'offset': 0x10000, 'erase_bytes': aligned, 'slot_bytes': 0x140000,
                'source_commit': commit, 'source_dirty': dirty,
                'application_preferences': {'partition': 'nvs', 'offset': 0x9000, 'bytes': 0x5000,
                    'namespace': 'bottie_ui_v1', 'key': 'prefs', 'record_version': 1,
                    'record_bytes': 12, 'backup_before_first_boot_required': True,
                    'explicit_erase_permitted': False, 'preserve_foreign_logical_contents': True},
                'configuration_sha256': hashlib.sha256(header.read_bytes()).hexdigest()}
    (private / 'candidate.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (private / 'candidate.json').chmod(0o600)
    print(json.dumps({key: value for key, value in manifest.items() if key != 'configuration_sha256'}))
    print('BOTTIE_PRIVATE_IMAGE_OK')
