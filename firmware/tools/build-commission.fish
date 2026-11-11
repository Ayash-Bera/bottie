#!/usr/bin/fish --no-config

if test (count $argv) -ne 1
    echo 'Usage: build-commission.fish /tmp/private-commission-directory' >&2
    exit 2
end
umask 077
set -l root (path resolve (path dirname (status filename))/../..)
set -l private (path resolve $argv[1])
python3 $root/firmware/tools/verify-private-build.py $private
or exit 1
set -l sdk $HOME/.local/share/bottie/esp-idf-v6.1
if test (git -C $sdk rev-parse HEAD) != fff9895c82d744c7237be8847347bdd1b07c6643
    echo 'Pinned SDK checkout mismatch.' >&2
    exit 1
end
set -gx IDF_TOOLS_PATH $HOME/.local/share/bottie/idf-tools-v6.1
set -gx IDF_PATH $sdk
set -gx IDF_CCACHE_ENABLE 0
set -e IDF_TARGET
source $sdk/export.fish
or exit 1
set -l output $private/build
idf.py -C $root/firmware -B $output -D IDF_TARGET=esp32s3 -D BOTTIE_HEADLESS=ON -D BOTTIE_RECOVERED_PANEL=ON -D BOTTIE_LIVE_COMPANION=ON -D BOTTIE_PRIVATE_CONFIG_DIR=$private -D SDKCONFIG=$output/sdkconfig -D "SDKCONFIG_DEFAULTS=$root/firmware/sdkconfig.defaults;$root/firmware/sdkconfig.live;$root/firmware/sdkconfig.commission" build > $private/build.log 2>&1
if test $status -ne 0
    echo "Build stopped; details remain private in $private/build.log" >&2
    exit 1
end
$HOME/.local/share/bottie/diagnostics-5.6.0/bin/python $root/scripts/verify_build.py $output esp32s3
or exit 1
python -m esp_idf_size --format json2 $output/bottie_smoke.map > $output/memory-report.json
or exit 1
python $root/firmware/tools/verify-live.py $output ON
or exit 1
python $root/firmware/tools/verify-private-build.py $private --image
or exit 1
echo BOTTIE_COMMISSION_BUILD_OK
