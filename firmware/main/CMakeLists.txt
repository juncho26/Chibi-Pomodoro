# The phone page (web/index.html) is stored gzipped inside the firmware. It is compressed here at
# build time, so editing web/index.html and rebuilding is all you need to do.
set(WEB_GZ "${CMAKE_BINARY_DIR}/index.html.gz")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    file(ARCHIVE_CREATE OUTPUT "${WEB_GZ}" PATHS "${CMAKE_CURRENT_LIST_DIR}/web/index.html"
         FORMAT raw COMPRESSION GZip COMPRESSION_LEVEL 9)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/web/index.html")
endif()

file(GLOB FONT_SRCS "fonts/*.c")
# LVGL, the screen driver and mDNS are listed in idf_component.yml; ESP-IDF downloads them and
# adds them here automatically.
idf_component_register(
    SRCS "main.c" "power.c" "powerlog.c" "lv_mem_psram.c" "imu.c" "rtc.c" "display.c" "store.c" "textfit.c" "net.c" "app.c" "ui.c" "character.c" "weather.c" "icons.c" ${FONT_SRCS}
    INCLUDE_DIRS "."
    EMBED_FILES "${WEB_GZ}"
    REQUIRES esp_lcd driver esp_driver_i2c esp_driver_gpio esp_driver_ledc esp_driver_spi esp_driver_usb_serial_jtag
             esp_adc esp_timer esp_wifi esp_netif esp_event esp_http_server json esp_http_client mbedtls nvs_flash lwip esp_pm)
target_compile_options(${COMPONENT_LIB} PRIVATE -DLV_LVGL_H_INCLUDE_SIMPLE -Wno-unused-function -Wno-format-truncation)

# LVGL allocates through lv_mem_psram.c (in this component): make sure the linker keeps it
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_malloc_core")
