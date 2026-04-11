cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(pse84_video_test)

target_sources(app PRIVATE src/main.c)

# Embed the raw RGB565 animation data into the firmware as a C include.
# The blob is 125 frames of 240x144 RGB565, packed as a single contiguous
# byte stream. generate_inc_file_for_target produces a comma-separated
# byte list suitable for #include inside an array initializer.
generate_inc_file_for_target(
	app
	src/frames.bin
	${ZEPHYR_BINARY_DIR}/include/generated/frames.bin.inc
)
