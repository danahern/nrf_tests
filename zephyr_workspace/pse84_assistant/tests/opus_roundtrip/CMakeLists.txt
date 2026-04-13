cmake_minimum_required(VERSION 3.20.0)

# Wire in the libopus Zephyr module so CONFIG_OPUS=y works here too.
list(APPEND ZEPHYR_EXTRA_MODULES
	"${CMAKE_CURRENT_SOURCE_DIR}/../../../modules/libopus")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(opus_roundtrip_test)

FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE
	${app_sources}
	${CMAKE_CURRENT_SOURCE_DIR}/../../src/opus_wrapper.c
)
target_include_directories(app PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

target_link_libraries(app PRIVATE opus)
