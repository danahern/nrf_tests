#!/bin/bash
# Build all firmware and run QEMU validation test
# Usage: ./run_qemu_test.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKSPACE="$SCRIPT_DIR/.."
NRF_ZEPHYR="$WORKSPACE/zephyrproject"
ALIF_SDK="$SCRIPT_DIR/../../sdk-alif"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

passed=0
failed=0

build_check() {
    local name="$1"
    local board="$2"
    local src="$3"
    local build_dir="$4"
    local workspace="$5"
    local env_prefix="$6"

    printf "  %-35s" "$name"
    if $env_prefix west build -b "$board" "$src" -d "$build_dir" -p 2>/dev/null | tail -1 | grep -q "Generating files"; then
        printf "${GREEN}BUILD OK${NC}\n"
        ((passed++))
    else
        printf "${RED}BUILD FAILED${NC}\n"
        ((failed++))
    fi
}

echo "========================================"
echo "Power Comparison - Build & QEMU Validation"
echo "========================================"

# --- nRF54LM20 Firmware ---
echo ""
echo "${YELLOW}Building nRF54LM20 firmware...${NC}"
cd "$NRF_ZEPHYR"

build_check "nrf54lm20_idle_test" \
    "nrf54lm20dk/nrf54lm20a/cpuapp" \
    "../nrf54lm20_idle_test" \
    "../nrf54lm20_idle_test/build" \
    "$NRF_ZEPHYR" \
    ""

build_check "nrf54lm20_adv_test" \
    "nrf54lm20dk/nrf54lm20a/cpuapp" \
    "../nrf54lm20_adv_test" \
    "../nrf54lm20_adv_test/build" \
    "$NRF_ZEPHYR" \
    ""

build_check "nrf54lm20_throughput_test" \
    "nrf54lm20dk/nrf54lm20a/cpuapp" \
    "../nrf54lm20_throughput_test" \
    "../nrf54lm20_throughput_test/build" \
    "$NRF_ZEPHYR" \
    ""

build_check "nrf54lm20_l2cap_test" \
    "nrf54lm20dk/nrf54lm20a/cpuapp" \
    "../nrf54lm20_l2cap_test" \
    "../nrf54lm20_l2cap_test/build" \
    "$NRF_ZEPHYR" \
    ""

# --- Alif B1 Firmware ---
echo ""
echo "${YELLOW}Building Alif B1 firmware...${NC}"
if [ -d "$ALIF_SDK" ]; then
    cd "$ALIF_SDK"
    ALIF_ENV="GNUARMEMB_TOOLCHAIN_PATH=/opt/homebrew ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb"

    for proj in alif_b1_idle_test alif_b1_adv_test alif_b1_throughput_test alif_b1_l2cap_test; do
        printf "  %-35s" "$proj"
        if env GNUARMEMB_TOOLCHAIN_PATH=/opt/homebrew ZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb \
            west build -b "alif_b1_dk/ab1c1f4m51820hh0/rtss_he" \
            "$WORKSPACE/$proj" -d "$WORKSPACE/$proj/build" -p 2>/dev/null | tail -1 | grep -q "Generating files"; then
            printf "${GREEN}BUILD OK${NC}\n"
            ((passed++))
        else
            printf "${RED}BUILD FAILED${NC}\n"
            ((failed++))
        fi
    done
else
    echo "  SKIP: sdk-alif not found at $ALIF_SDK"
fi

# --- QEMU Validation (3 CPU targets) ---
echo ""
echo "${YELLOW}Building & running QEMU validation...${NC}"
cd "$NRF_ZEPHYR"

# Cortex-M33 (matches nRF54LM20 architecture)
printf "  %-35s" "QEMU Cortex-M33 (mps2-an521)"
if west build -b mps2/an521/cpu0 "../power_comparison/sim_test" -d "../power_comparison/sim_test/build_m33" -p 2>/dev/null | tail -1 | grep -q "Generating files"; then
    QEMU_OUT=$(timeout 25 qemu-system-arm -cpu cortex-m33 -machine mps2-an521 -nographic -vga none -net none -serial mon:stdio \
        -kernel "$WORKSPACE/power_comparison/sim_test/build_m33/zephyr/zephyr.elf" 2>&1 || true)
    if echo "$QEMU_OUT" | grep -q "ALL TESTS PASSED"; then
        printf "${GREEN}ALL TESTS PASSED${NC}\n"
        ((passed++))
    else
        printf "${RED}TESTS FAILED${NC}\n"
        ((failed++))
    fi
else
    printf "${RED}BUILD FAILED${NC}\n"
    ((failed++))
fi

# Cortex-M55 (matches Alif B1 architecture)
printf "  %-35s" "QEMU Cortex-M55 (mps3-an547)"
if west build -b mps3/corstone300/an547 "../power_comparison/sim_test" -d "../power_comparison/sim_test/build_m55" -p 2>/dev/null | tail -1 | grep -q "Generating files"; then
    QEMU_OUT=$(timeout 25 qemu-system-arm -cpu cortex-m55 -machine mps3-an547 -nographic -vga none -net none -serial mon:stdio \
        -kernel "$WORKSPACE/power_comparison/sim_test/build_m55/zephyr/zephyr.elf" 2>&1 || true)
    if echo "$QEMU_OUT" | grep -q "ALL TESTS PASSED"; then
        printf "${GREEN}ALL TESTS PASSED${NC}\n"
        ((passed++))
    else
        printf "${RED}TESTS FAILED${NC}\n"
        ((failed++))
    fi
else
    printf "${RED}BUILD FAILED${NC}\n"
    ((failed++))
fi

# --- Summary ---
echo ""
echo "========================================"
total=$((passed + failed))
echo "Total: $passed/$total passed"
if [ $failed -eq 0 ]; then
    printf "${GREEN}ALL CHECKS PASSED${NC}\n"
else
    printf "${RED}$failed CHECK(S) FAILED${NC}\n"
fi
echo "========================================"

exit $failed
