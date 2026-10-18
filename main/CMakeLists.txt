idf_component_register(
    SRCS "main.c" "game.c" "fc_count.c" "fc_demo.c" "fc_settings.c" "fc_store_nvs.c"
         "fc_sfx_device.c" "rtttl_player.c" "cJSON.c" "gk_cjk_font.c"
         "fc_camera.c" "fc_net.c" "fc_runtime.c"
         "fc_provider_http.c" "fc_provider_vlm.c"
         "fc_time_device.c"
    INCLUDE_DIRS "."
    REQUIRES mosaico_game_app mosaico_raylib_fast esp-gsp nvs_flash
             esp-mosaico-bsp esp_codec_dev mosaico_module_camera
    PRIV_REQUIRES esp_wifi esp_netif esp_event esp_http_server esp_http_client
                  esp_driver_jpeg esp_timer mbedtls esp_hw_support heap)

# GSP needs an image to bind the Raylib framebuffer. This generated placeholder
# is embedded in the application; no external game resource partition is used.
set(canvas_png "${CMAKE_CURRENT_BINARY_DIR}/canvas.png")
execute_process(COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/make_canvas_png.py"
    "${canvas_png}" RESULT_VARIABLE canvas_result)
if(NOT canvas_result EQUAL 0)
    message(FATAL_ERROR "Could not generate the game canvas placeholder")
endif()
set(CANVAS_IMAGE "${canvas_png}")
configure_file("scene.json.in" "${CMAKE_CURRENT_BINARY_DIR}/scene.json" @ONLY)
gsp_add_bundle(${COMPONENT_LIB}
    SCENES "${CMAKE_CURRENT_BINARY_DIR}/scene.json"
    PIXEL_FORMAT rgb565)
