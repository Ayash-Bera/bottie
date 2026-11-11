#!/usr/bin/fish --no-config

set -l root (path resolve (path dirname (status filename))/../..)
set -l output $root/build/firmware/native
if test (count $argv) -gt 0
    set output $argv[1]
end
cmake -S $root/firmware/native -B $output -DCMAKE_BUILD_TYPE=Debug -DBOTTIE_SANITIZE=ON
or exit 1
cmake --build $output --parallel 2
or exit 1
ctest --test-dir $output --output-on-failure
or exit 1
echo BOTTIE_NATIVE_OK
