#!/usr/bin/fish --no-config

set -l root (path resolve (path dirname (status filename))/..)
if not test -f $root/firmware/tools/check-native.fish
    echo 'Firmware core is missing. Integrate the verified firmware commit first.' >&2
    exit 1
end

fish --no-config $root/firmware/tools/check-native.fish
or exit 1
fish --no-config $root/firmware/network/tools/check-native.fish
or exit 1

set -l scratch (mktemp -d /tmp/bottie-integration-build.XXXXXX)
or exit 1
function cleanup --on-event fish_exit --inherit-variable scratch
    rm -rf -- $scratch
end

cd $root/companion
or exit 1
go build -o $scratch/server ./cmd/server
or exit 1
go build -o $scratch/integration ./cmd/integration
or exit 1
$scratch/integration -server $scratch/server -native $root/build/firmware/native/bottie_fixture -fixtures $root/protocol/fixtures
or exit 1
