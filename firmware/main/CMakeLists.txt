idf_component_register(
    SRCS
        "app_main.c"
        "kvm_config.c"
        "kvm_input.c"
        "kvm_wifi.c"
        "kvm_ble.c"
        "kvm_net.c"
        "kvm_wan.c"
        "kvm_captive.c"
        "kvm_crypto.c"
        "kvm_http.c"
    INCLUDE_DIRS "."
    EMBED_FILES "index.html"
    EMBED_TXTFILES "relay_ca.pem"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        esp_timer
        esp-tls
        json
        mbedtls
        bt
        esp_hid
        driver
)


