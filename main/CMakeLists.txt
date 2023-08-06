idf_component_register(SRCS "init_UART.c" "http_requester.c" "get_adc.c" "main.c" "http_server.c" "wifi_app.c" "app_nvs.c"
                    INCLUDE_DIRS "."
                    EMBED_FILES webpage/app.css webpage/app.js webpage/favicon.ico webpage/index.html webpage/jquery-3.3.1.min.js certificate.pem mqtt-cert.pem)
