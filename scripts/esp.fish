#!/usr/bin/fish --no-config

set -l root (path resolve (path dirname (status filename))/..)
set -l source_root $root/firmware
set -l base $HOME/.local/share/bottie
set -l sdk $base/esp-idf-v6.1
set -l expected_commit fff9895c82d744c7237be8847347bdd1b07c6643
set -l diagnostic_python $base/diagnostics-5.6.0/bin/python

if test (count $argv) -lt 1
    echo 'Usage: fish --no-config scripts/esp.fish {versions|identify PORT|build|verify-build|smoke-build TARGET}' >&2
    exit 2
end

switch $argv[1]
    case identify
        if test (count $argv) -ne 2
            echo 'Usage: fish --no-config scripts/esp.fish identify PORT' >&2
            exit 2
        end
        set -l port $argv[2]
        if not test -c $port; or not test -r $port; or not test -w $port
            echo 'Serial device is not accessible. See docs/hardware-status.md.' >&2
            exit 1
        end
        exec $diagnostic_python -m esptool --port $port --baud 115200 --before default-reset --after hard-reset --no-stub --connect-attempts 2 flash-id
    case build verify-build
        if not test -s $root/device-target.local
            echo 'Device target is unverified. Run identify and record its confirmed target before a device build.' >&2
            exit 1
        end
        set -g build_target (string trim < $root/device-target.local)
        if test (count $build_target) -ne 1; or not string match -rq '^esp32[a-z0-9]*$' -- $build_target
            echo 'Invalid device target.' >&2
            exit 1
        end
        set -g build_dir $root/build/firmware/device-$build_target
    case smoke-build
        if test (count $argv) -ne 2; or not contains -- $argv[2] esp32c3 esp32s3
            echo 'Smoke target must be esp32c3 or esp32s3. This does not identify the device.' >&2
            exit 2
        end
        set -g build_target $argv[2]
        set -g build_dir $root/build/firmware/smoke-$build_target
        echo "Toolchain smoke target: $build_target. Device compatibility is unverified."
    case versions
    case '*'
        echo 'Unknown command. Flashing is not part of this wrapper.' >&2
        exit 2
end

set -l actual_commit (git -C $sdk rev-parse HEAD 2>/dev/null)
if test "$actual_commit" != "$expected_commit"
    echo 'ESP-IDF checkout is missing or does not match the pinned commit. See README.md.' >&2
    exit 1
end

set -gx IDF_TOOLS_PATH $base/idf-tools-v6.1
set -gx IDF_PATH $sdk
set -gx IDF_CCACHE_ENABLE 0
set -e IDF_TARGET
source $sdk/export.fish
or exit 1
cd $root
or exit 1

switch $argv[1]
    case versions
        python --version; or exit 1
        idf.py --version; or exit 1
        $diagnostic_python -c 'import esptool; assert esptool.__version__ == "5.6.0"; print("esptool", esptool.__version__)'; or exit 1
        xtensa-esp-elf-gcc --version; or exit 1
        riscv32-esp-elf-gcc --version; or exit 1
        cmake --version; or exit 1
        ninja --version; or exit 1
        test (xtensa-esp-elf-gcc -dumpfullversion) = 15.2.0; or exit 1
        test (riscv32-esp-elf-gcc -dumpfullversion) = 15.2.0; or exit 1
        test (cmake --version | head -n 1) = 'cmake version 4.0.3'; or exit 1
        test (ninja --version) = 1.12.1; or exit 1
        echo TOOL_VERSIONS_OK
    case build smoke-build verify-build
        if test $argv[1] != verify-build
            mkdir -p $build_dir
            or exit 1
            idf.py -C $source_root -B $build_dir -D IDF_TARGET=$build_target -D SDKCONFIG=$build_dir/sdkconfig build
            or exit 1
        end
        $diagnostic_python $root/scripts/verify_build.py $build_dir $build_target
        or exit 1
        if test $argv[1] = verify-build
            echo CONFIRMED_TARGET_BUILD_OK
        end
end
