# vittil comes from main/idf_component.yml, which makes it a requirement of
# main by itself.
idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES nvs_flash esp_netif esp_event esp_driver_gpio
                                     protocol_examples_common)
