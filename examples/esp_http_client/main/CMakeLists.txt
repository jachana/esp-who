set(requires "")

idf_build_get_property(target IDF_TARGET)

idf_component_register(SRCS "esp_http_client_example.c"
                       INCLUDE_DIRS "."
                       REQUIRES ${requires}
                       EMBED_TXTFILES connect_auras_root_cert.pem
                      )
