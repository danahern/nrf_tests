cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(framing_test)

# Pull in the framing TU directly. No Zephyr/libopus deps — this suite
# runs cleanly on native_sim with nothing else checked out.
FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE
	${app_sources}
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/framing.c
)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../src
)
