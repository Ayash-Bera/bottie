import pathlib
import stat
import sys

network = pathlib.Path(sys.argv[1]).resolve()
output = pathlib.Path(sys.argv[2]).resolve()
if not str(output).startswith('/tmp/bottie-network-idf-'):
    raise SystemExit('Check project must be in /tmp/bottie-network-idf-*')
output.mkdir(mode=0o700, parents=True, exist_ok=True)
if stat.S_IMODE(output.stat().st_mode) & 0o077:
    raise SystemExit('Check directory must be private (mode0700)')
(output / 'main').mkdir(exist_ok=True)
(output / 'CMakeLists.txt').write_text(
    'cmake_minimum_required(VERSION 3.22)\n'
    f'set(EXTRA_COMPONENT_DIRS "{network}" "{network.parent}/components/bottie_core")\n'
    'include($ENV{IDF_PATH}/tools/cmake/project.cmake)\nproject(network_check)\n')
(output / 'main/CMakeLists.txt').write_text(
    'idf_component_register(SRCS "main.cpp" PRIV_REQUIRES network bottie_core)\n')
(output / 'main/main.cpp').write_text('''#include "bottie/network.hpp"
#include <cstring>
static volatile bool exercise = false;
static uint16_t framebuffer[240 * 296];
static uint8_t renderer_art[96 * 96 * 2], handoff_art[96 * 96 * 2];
static char json[bottie::max_json_bytes + 1];
extern "C" void app_main() {
    bottie::network::Client client;
    if (exercise) {
        client.start();
        size_t length;
        client.playback(json, sizeof json, length);
        client.control("{\\"action\\":\\"pause\\"}", 18);
        const auto* image = client.artwork();
        bottie::Artwork view;
        if (image) {
            size_t bytes = image->stride * image->height;
            if (bytes > sizeof handoff_art) return;
            std::memcpy(handoff_art, image->pixels, bytes);
            std::memcpy(renderer_art, handoff_art, bytes);
            view = {renderer_art, image->width, image->height, image->stride};
        }
        bottie::Model model;
        model.ingest(json, length, 0);
        bottie::render(model, 0, {framebuffer, 240 * 296, 240, 296, 240}, image ? &view : nullptr);
        client.cancel();
        client.stop();
    }
}
''')
(output / 'sdkconfig.defaults').write_text(
    'CONFIG_BOTTIE_NETWORK_ENABLED=y\n'
    'CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y\n'
    'CONFIG_PARTITION_TABLE_SINGLE_APP=y\n'
    'CONFIG_COMPILER_OPTIMIZATION_SIZE=y\n'
    'CONFIG_LOG_DEFAULT_LEVEL_WARN=y\n'
    '# CONFIG_SPIRAM is not set\n')
