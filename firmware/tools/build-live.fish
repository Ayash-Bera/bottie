#!/usr/bin/fish --no-config

set -l panel OFF
set -l profile live-headless
if test (count $argv) -gt 0
    if test (count $argv) -ne 1; or test $argv[1] != recovered
        echo 'Usage: build-live.fish [recovered]' >&2
        exit 2
    end
    set panel ON
    set profile live-recovered
end
set -l root (path resolve (path dirname (status filename))/../..)
set -l sdk $HOME/.local/share/bottie/esp-idf-v6.1
if test (git -C $sdk rev-parse HEAD) != fff9895c82d744c7237be8847347bdd1b07c6643
    echo 'Pinned SDK checkout mismatch.' >&2
    exit 1
end
if not test -f $root/firmware/network/CMakeLists.txt
    echo 'Verified network component is required.' >&2
    exit 1
end
set -gx IDF_TOOLS_PATH $HOME/.local/share/bottie/idf-tools-v6.1
set -gx IDF_PATH $sdk
set -gx IDF_CCACHE_ENABLE 0
set -e IDF_TARGET
source $sdk/export.fish
or exit 1
set -l output $root/build/firmware/$profile-esp32s3
mkdir -p $output
or exit 1
idf.py -C $root/firmware -B $output -D IDF_TARGET=esp32s3 -D BOTTIE_HEADLESS=ON -D BOTTIE_RECOVERED_PANEL=$panel -D BOTTIE_LIVE_COMPANION=ON -D SDKCONFIG=$output/sdkconfig -D "SDKCONFIG_DEFAULTS=$root/firmware/sdkconfig.defaults;$root/firmware/sdkconfig.live" build
or exit 1
$HOME/.local/share/bottie/diagnostics-5.6.0/bin/python $root/scripts/verify_build.py $output esp32s3
or exit 1
python -m esp_idf_size --format json2 $output/bottie_smoke.map > $output/memory-report.json
or exit 1
python $root/firmware/tools/verify-live.py $output $panel
or exit 1
echo BOTTIE_LIVE_BUILD_OK
