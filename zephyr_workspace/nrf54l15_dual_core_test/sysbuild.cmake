# Sysbuild configuration for dual-core nRF54L15 application
# This builds both the ARM Cortex-M33 (cpuapp) and RISC-V (cpuflpr) cores

# Declare RISC-V remote core image
set(FLPR_BOARD nrf54l15dk/nrf54l15/cpuflpr)

ExternalZephyrProject_Add(
    APPLICATION cpuflpr
    SOURCE_DIR ${APP_DIR}/cpuflpr
    BOARD ${FLPR_BOARD}
)

# Add dependency so cpuapp waits for cpuflpr to be built
add_dependencies(${DEFAULT_IMAGE} cpuflpr)
