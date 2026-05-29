idf_component_register(SRCS "rmaker_file_upload.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_http_client esp-tls json_parser
                    PRIV_REQUIRES esp_rainmaker rmaker_common)
