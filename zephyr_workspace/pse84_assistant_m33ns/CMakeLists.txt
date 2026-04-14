# PSE84 Voice Assistant — M33 companion.
#
# This is the M33 image that replaces samples/basic/minimal in the
# kit_pse84_eval sysbuild companion slot. Its jobs today are:
#
#   1. Drive ifx_pse84_cm55_startup() via the SoC's soc_late_init_hook
#      (CONFIG_SOC_PSE84_M55_ENABLE=y + CONFIG_PSE84_MAIN_THREAD_DISABLED=n).
#   2. Set CLK_HF12 to a useful frequency for the MIPI-DSI D-PHY PLL.
#   3. Keep the M33 running after CM55 handoff so we can eventually
#      host the Bluetooth stack + ipc_service peer here.
#
# Phase 0b.7: skeleton only — main() just logs a heartbeat so we can
# verify the new image runs where samples/basic/minimal used to.
# Phase 4+: bring up CONFIG_BT + HCI UART + AIROC firmware download.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pse84_assistant_m33)

target_sources(app PRIVATE src/main.c)
