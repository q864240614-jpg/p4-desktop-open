file(GLOB FONT_SOURCES "lv_font_*.c")
idf_component_register(SRCS "portable_ui.c" "bambu_state.c" "portable_ui_json.c" "portable_icons.c"
    ${FONT_SOURCES} INCLUDE_DIRS "." REQUIRES lvgl json esp_hw_support heap)

# The cached alpha and LED raster loops run on the display task.
target_compile_options(${COMPONENT_LIB} PRIVATE -O3)
