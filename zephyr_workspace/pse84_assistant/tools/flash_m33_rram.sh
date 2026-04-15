#!/bin/bash
# Flash the M33 companion to the RRAM slot that extended-boot actually
# reads (0x22011000 SBUS NS alias, openocd bank #0 main_ns).
#
# Zephyr's in-tree mcuboot imgtool produces a --hex-addr 0x60100000
# (SMIF1) image that extended-boot never jumps to on this kit. Our
# sysbuild overlay puts m33s_header at 0x22011000, but imgtool still
# produces the wrong byte-layout (0xff padding + no trailing TLVs).
# Extended-boot rejects that. The pse84_display recipe (see its
# LEARNINGS.md) uses edgeprotecttools image-metadata --erased-val 0 —
# that produces a header layout extended-boot accepts.
#
# Usage:  ./tools/flash_m33_rram.sh <build_dir>
# Example: ./tools/flash_m33_rram.sh /tmp/build_hw
set -e
BUILD="${1:-build_hw}"
HEX="${BUILD}/enable_cm55/zephyr/zephyr.hex"
OUT="/tmp/m33_rram.hex"

if [ ! -f "$HEX" ]; then
  echo "ERROR: $HEX not found. Build pse84_assistant first." >&2
  exit 1
fi

echo "=== edgeprotecttools sign: $HEX -> $OUT ==="
edgeprotecttools image-metadata \
  -i "$HEX" -o "$OUT" \
  --erased-val 0 \
  --hex-addr 0x22011000 \
  --header-size 0x400 \
  --slot-size 0x59400

OCD_SUPPORT=zephyr_workspace/zephyrproject/zephyr/boards/infineon/kit_pse84_eval/support
OCD_MTB=/Applications/ModusToolboxProgtools-1.7/openocd

echo "=== openocd flash write_image (RRAM bank #0) ==="
"$OCD_MTB/bin/openocd" \
  -s "$OCD_SUPPORT" \
  -s "$OCD_MTB/scripts" \
  -f "$OCD_SUPPORT/openocd.cfg" \
  -c 'init' \
  -c 'targets cat1d.cm33' \
  -c 'reset halt' \
  -c "flash write_image erase $OUT" \
  -c 'reset run' \
  -c 'shutdown'
