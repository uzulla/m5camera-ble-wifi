idf_component_register(
    SRCS "main.c" "snap.c" "wifi_mgr.c" "http_srv.c" "ble_snap.c" "cpu_mon.c" "i2c_snap.c" "status_adv.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_http_server esp_wifi esp_netif esp_event nvs_flash esp_timer bt esp_driver_i2c esp_driver_gpio
)
