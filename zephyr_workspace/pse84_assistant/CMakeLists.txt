cmake_minimum_required(VERSION 3.20.0)

# Track C: register the out-of-tree libopus Zephyr module before
# find_package(Zephyr) so its Kconfig + CMakeLists are picked up.
list(APPEND ZEPHYR_EXTRA_MODULES
    "${CMAKE_CURRENT_SOURCE_DIR}/../modules/libopus")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pse84_assistant)

target_sources(app PRIVATE
    src/main.c
    src/state.c
    src/framing.c
    src/opus_wrapper.c
    src/audio.c
)

# ui.c pulls in LVGL headers unconditionally; only compile it when
# CONFIG_LVGL=y. The QEMU M55 smoke-test build
# (boards/mps3_corstone300_an547 + prj_qemu_m55.conf) disables LVGL to
# stay inside the MPS3 board's displayless capability; main.c gates the
# UI hooks with #ifdef CONFIG_LVGL so the same state_* + framing +
# opus paths still exercise.
if(CONFIG_LVGL)
    target_sources(app PRIVATE src/ui.c)
endif()

# Snapshot helper (native_sim only; see src/snapshot.h).
if(CONFIG_APP_SNAPSHOT)
    target_sources(app PRIVATE src/snapshot.c)
endif()

target_include_directories(app PRIVATE src)

# Track C: link the libopus INTERFACE target. No-op when CONFIG_OPUS=n.
target_link_libraries(app PRIVATE opus)
