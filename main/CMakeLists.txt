idf_component_register(
    SRCS "main.c" "variant.c" "settings.c" "http_util.c" "slicer.c" "wifi.c" "storage.c" "usb_serial.cpp" "printer.c" "axidraw.c" "web.c"
    INCLUDE_DIRS "."
    EMBED_FILES "www/index.html" "www/i18n.js"
                "www/fonts/space-grotesk.woff2" "www/fonts/jetbrains-mono.woff2")

# Kennung der Variante im Image behalten, auch wenn sie nirgends direkt benutzt würde
target_link_libraries(${COMPONENT_LIB} "-u uprint_desc")
