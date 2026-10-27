# MorseLink firmware component. The pure-C modules (morse, hseq, calib,
# decoder, peers, convo, proto, link, disc, phone, ttest, bench) are also
# compiled by the host tests in test/host, so they must not include ESP-IDF
# headers.
idf_component_register(
    SRCS
        "app_main.c"
        "morse.c"
        "hseq.c"
        "calib.c"
        "decoder.c"
        "peers.c"
        "convo.c"
        "proto.c"
        "link.c"
        "disc.c"
        "phone.c"
        "ttest.c"
        "bench.c"
        "pmic.c"
        "power.c"
        "haptics.c"
        "keyer.c"
        "store.c"
        "radio.c"
        "radio_sx1262.c"
        "net.c"
        "ntest.c"
        "ble.c"
        "ui.c"
        "console.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        bt
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_ledc
        esp_driver_spi
        esp_timer
        nvs_flash
        console
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
