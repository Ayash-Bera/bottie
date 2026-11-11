idf_component_register(SRCS "client.cpp" "policy.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES bottie_core
                       PRIV_REQUIRES esp_wifi esp_netif esp_event esp-tls esp_timer)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
