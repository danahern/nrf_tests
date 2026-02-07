# Dual-core BLE + RISC-V test for nRF54L15
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nrf54l15_dual_core_test)

# Add cpuapp (ARM Cortex-M33) sources
target_sources(app PRIVATE cpuapp/src/main.c)
