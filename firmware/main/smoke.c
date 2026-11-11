#include <inttypes.h>
#include <stdio.h>

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "sdkconfig.h"

#if !CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#error "This smoke app requires the native USB Serial/JTAG console."
#endif

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    printf("Bottie smoke ready\n");
    printf("Target: %s; SDK: %s\n", CONFIG_IDF_TARGET, esp_get_idf_version());
    printf("Cores: %u; revision: %u.%u\n", (unsigned)chip.cores,
           (unsigned)(chip.revision / 100), (unsigned)(chip.revision % 100));

    uint32_t flash_bytes = 0;
    esp_err_t error = esp_flash_get_size(NULL, &flash_bytes);
    if (error == ESP_OK) {
        printf("Flash: %" PRIu32 " bytes\n", flash_bytes);
    } else {
        printf("Flash size unavailable: %s\n", esp_err_to_name(error));
    }

    printf("Minimum free heap: %" PRIu32 " bytes\n", esp_get_minimum_free_heap_size());
    printf("Smoke app idle\n");
    fflush(stdout);
}
