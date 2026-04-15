cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mbox_pse84_test)

# Route drivers/mbox/mbox_pse84.c through the fake Cy_IPC_Drv_* shim.
# The driver is pulled in via CONFIG_MBOX_PSE84=y; we just have to put
# the fake include dir on the zephyr-wide include path so the driver
# TU resolves <cy_ipc_drv.h> to our shim instead of the (absent on
# native_sim) hal_infineon PDL.
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/fake_cy_ipc)

target_sources(app PRIVATE
	src/test_mbox_pse84.c
	fake_cy_ipc/cy_ipc_drv.c
)
