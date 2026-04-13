cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pse84_assistant)

target_sources(app PRIVATE
    src/main.c
    src/state.c
    src/ui.c
)

target_include_directories(app PRIVATE src)
