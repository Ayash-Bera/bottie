#!/usr/bin/fish --no-config
set -l network (path resolve (path dirname (status filename))/..)
set -l output (mktemp -d /tmp/bottie-network-native-XXXXXX)
or exit 1
c++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -g -D_GLIBCXX_ASSERTIONS -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I$network/include -I$network/../core/include $network/policy.cpp $network/text_digest.cpp $network/tests/policy_tests.cpp \
    $network/../core/text_mask.cpp $network/../core/artwork.cpp $network/../core/state.cpp $network/../core/parser.cpp $network/../core/detail.cpp -lcrypto -o $output/network-tests
or exit 1
$output/network-tests
or exit 1
