idf_component_register(
    SRCS "main.c" "settings.c" "wifi.c" "storage.c" "usb_serial.cpp" "printer.c" "web.c"
    INCLUDE_DIRS "."
    EMBED_FILES "www/index.html" "www/i18n.js"
                "www/fonts/space-grotesk.woff2" "www/fonts/jetbrains-mono.woff2")
