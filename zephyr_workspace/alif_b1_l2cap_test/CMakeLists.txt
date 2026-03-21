cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alif_b1_l2cap_test)

set(ALIF_BLE_COMMON ${ZEPHYR_BASE}/../alif/samples/bluetooth/common)

target_include_directories(app PRIVATE ${ALIF_BLE_COMMON})
target_sources(app PRIVATE
    src/main.c
    ${ALIF_BLE_COMMON}/batt_svc.c
    ${ALIF_BLE_COMMON}/address_verification.c
)
