idf_component_register(SRCS "client.cpp" "policy.cpp" "text_digest.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES bottie_core
                       PRIV_REQUIRES esp_wifi esp_netif esp_event esp-tls esp_timer mbedtls)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -fstack-usage)
if(BOTTIE_PRIVATE_CONFIG_DIR)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${BOTTIE_PRIVATE_CONFIG_DIR}")
endif()
