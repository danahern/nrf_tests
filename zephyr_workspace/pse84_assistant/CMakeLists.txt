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
    src/ui.c
    src/framing.c
    src/opus_wrapper.c
)

target_include_directories(app PRIVATE src)

# Track C: link the libopus INTERFACE target. No-op when CONFIG_OPUS=n.
target_link_libraries(app PRIVATE opus)
