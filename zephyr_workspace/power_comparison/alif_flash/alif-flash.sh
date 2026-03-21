#!/bin/bash
# Wrapper to run Alif security tools (Linux x86-64) via Docker on macOS
# Usage: ./alif-flash.sh <firmware.bin> [device_part]
#
# This script:
# 1. Configures the Linux tools for B1
# 2. Generates the TOC package
# 3. Writes to MRAM via SE-UART (requires serial port forwarding)

set -e

FIRMWARE="${1:?Usage: alif-flash.sh <firmware.bin> [device_part]}"
DEVICE_PART="${2:-AB1C1F4M51820HH}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SETOOLS_LINUX="$(cd "$SCRIPT_DIR/../../alif_security_tools/app-release-exec-linux" && pwd)"

if [ ! -f "$FIRMWARE" ]; then
    echo "ERROR: Firmware file not found: $FIRMWARE"
    exit 1
fi

FW_BASENAME=$(basename "$FIRMWARE")

echo "=== Alif B1 Flash Tool (Docker) ==="
echo "Firmware: $FIRMWARE"
echo "Device:   $DEVICE_PART"
echo ""

# Step 1: Configure tools and generate TOC via Docker
echo "Generating TOC package..."
docker run --rm --platform linux/amd64 \
  -v "$SETOOLS_LINUX":/tools \
  -v "$(dirname "$FIRMWARE")":/fw \
  ubuntu:22.04 \
  bash -c "
cd /tools

# Configure for B1 (Balletto -> B1 AB1C1F4M51820HH)
printf '1\n2\n2\n5\n' | ./tools-config > /dev/null 2>&1

# Copy firmware
cp /fw/$FW_BASENAME build/images/app_firmware.bin

# Write app-cfg.json
cat > build/config/app-cfg.json << 'EOF'
{
    \"DEVICE\": {
        \"disabled\": false,
        \"binary\": \"app-device-config.json\",
        \"version\": \"0.5.00\",
        \"signed\": true
    },
    \"APP-HE\": {
        \"disabled\": false,
        \"binary\": \"app_firmware.bin\",
        \"version\": \"1.0.0\",
        \"signed\": true,
        \"cpu_id\": \"M55_HE\",
        \"loadAddress\": \"0x80000000\",
        \"flags\": [\"load\", \"boot\"]
    }
}
EOF

# Generate TOC
./app-gen-toc 2>&1
echo 'TOC_RESULT=\$?'
"

echo ""
echo "TOC package generated at: $SETOOLS_LINUX/build/AppTocPackage.bin"
echo ""

# Step 2: Write to MRAM
# Docker Desktop on macOS can't pass through USB serial ports directly.
# The user needs to run the write-mram step from a Linux machine or VM,
# or use socat to bridge the serial port.
echo "=== MANUAL STEP REQUIRED ==="
echo "Docker can't access macOS serial ports directly."
echo "To flash, run this on a Linux machine (or in a VM with USB passthrough):"
echo ""
echo "  cd $SETOOLS_LINUX"
echo "  ./app-write-mram"
echo ""
echo "Or use the macOS tools if they support B1:"
echo "  cd $(dirname "$SETOOLS_LINUX")/app-release-exec-macos"
echo "  ./app-write-mram"
