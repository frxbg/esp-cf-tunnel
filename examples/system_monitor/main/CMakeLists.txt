set(CF_PATH "${CMAKE_CURRENT_LIST_DIR}/../../../components/esp_cf_tunnel")
set(ASSET_C "${CMAKE_CURRENT_BINARY_DIR}/monitor_assets.c")
idf_component_register(SRCS main.c monitor_store.c monitor_network.c monitor_http.c monitor_api.c monitor_remote.c monitor_io.c monitor_led.c monitor_tunnel.c "${ASSET_C}"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "${CF_PATH}/src/protocol" "${CF_PATH}/third_party/cjson"
    REQUIRES esp_cf_tunnel esp_http_server esp_wifi esp_netif esp_event nvs_flash
        esp_timer esp_driver_gpio esp_driver_tsens spi_flash esp_psram esp_app_format led_strip)
add_custom_command(OUTPUT "${ASSET_C}"
    COMMAND ${PYTHON} "${CMAKE_CURRENT_LIST_DIR}/../../../tools/embed_monitor_assets.py"
        "${CMAKE_CURRENT_LIST_DIR}/../web" "${ASSET_C}"
    DEPENDS ../web/index.html ../web/style.css ../web/app.js ../web/favicon.svg
        ../../../tools/embed_monitor_assets.py
    VERBATIM)
