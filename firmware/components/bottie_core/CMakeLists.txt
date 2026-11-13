idf_component_register(SRCS "../../core/parser.cpp" "../../core/state.cpp" "../../core/artwork.cpp" "../../core/text_mask.cpp" "../../core/render.cpp" "../../core/dirty.cpp" "../../core/interaction.cpp"
                       INCLUDE_DIRS "../../core/include")
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -fstack-usage)
