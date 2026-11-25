# esp-bringup: first-boot and recovery setup (Wi-Fi, hotspot with captive portal, per-project
# settings page) for ESP-IDF devices.
idf_component_register(
    SRCS "bringup.c" "bringup_wifi.c" "bringup_dns.c" "bringup_http.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "www/setup.html"
    # esp_http_server is public: bringup.h exposes httpd_handle_t / httpd_req_t
    REQUIRES esp_http_server
    PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer nvs_flash lwip json mbedtls log)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -Wno-missing-field-initializers)
