cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nrf54l15_gatt_peripheral_fast)

target_sources(app PRIVATE src/main.c)
