#!/usr/bin/env fish
set -l companion_root (path resolve (status dirname)/..)
set -l runtime $HOME/.local/share/bottie/host-runtime
set -l units $HOME/.config/systemd/user
if test (count $argv) -gt 1
    echo 'Usage: fish --no-config companion/tools/install-user.fish [--start]' >&2
    exit 2
end
if test (count $argv) -eq 1; and test "$argv[1]" != --start
    echo 'Usage: fish --no-config companion/tools/install-user.fish [--start]' >&2
    exit 2
end
for required in $runtime/.env $runtime/gateway.env $runtime/device-token
    if not test -f $required; or test -L $required; or test (stat -c %a $required) != 600
        echo 'Prepare the documented private runtime configuration with mode 0600 first.' >&2
        exit 1
    end
end
if test -L $runtime; or test (stat -c %a $runtime) != 700
    echo 'The runtime directory must have mode 0700.' >&2
    exit 1
end
if test -L $runtime/bin
    echo 'The runtime binary directory must not be a symbolic link.' >&2
    exit 1
end
pkg-config --exists libsecret-1; or exit 1
set -l stage (mktemp -d /tmp/bottie-host-install-XXXXXXXX); or exit 1
function cleanup_stage --on-event fish_exit --inherit-variable stage
    rm -rf -- $stage
end
pushd $companion_root; or exit 1
env CGO_ENABLED=1 go build -mod=readonly -o $stage/companion ./cmd/server; or exit 1
env CGO_ENABLED=1 go build -mod=readonly -o $stage/device-bridge ./cmd/device-bridge; or exit 1
popd
systemd-analyze --user verify $companion_root/deploy/bottie-companion.service $companion_root/deploy/bottie-gateway.service; or exit 1
install -d -m 700 $runtime/bin; or exit 1
mkdir -p $units; or exit 1
for program in companion device-bridge
    install -m 700 $stage/$program $runtime/bin/$program.next; or exit 1
    mv -f $runtime/bin/$program.next $runtime/bin/$program; or exit 1
end
for unit in bottie-companion.service bottie-gateway.service
    install -m 600 $companion_root/deploy/$unit $units/$unit; or exit 1
end
systemctl --user daemon-reload; or exit 1
systemctl --user enable bottie-companion.service bottie-gateway.service; or exit 1
if contains -- --start $argv
    systemctl --user restart bottie-companion.service bottie-gateway.service; or exit 1
end
echo 'Bottie binaries and login-started user services installed. No lingering enabled.'
