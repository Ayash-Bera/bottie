if(BOTTIE_HEADLESS)
    set(app_sources "headless.cpp" "../board/simulated.cpp")
else()
    set(app_sources "smoke.c")
endif()
idf_component_register(SRCS ${app_sources}
                       PRIV_REQUIRES spi_flash bottie_core esp_timer
                       INCLUDE_DIRS "../board")
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
