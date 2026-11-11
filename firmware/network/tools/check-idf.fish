#!/usr/bin/fish --no-config
set -l network (path resolve (path dirname (status filename))/..)
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
set -l output $BOTTIE_NETWORK_CHECK_DIR
if test -z "$output"
    set output (mktemp -d /tmp/bottie-network-idf-XXXXXX)
end
python $network/tools/make-check-project.py $network $output
or exit 1
idf.py -C $output -B $output/build -D IDF_TARGET=esp32s3 -D SDKCONFIG=$output/sdkconfig build
or exit 1
idf.py -C $output -B $output/build size
or exit 1
echo "Check output: $output"
echo BOTTIE_NETWORK_IDF_OK
