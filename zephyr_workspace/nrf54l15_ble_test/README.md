# nRF54L15 BLE Throughput Test

A Zephyr RTOS application for measuring BLE streaming performance and MIPS on the nRF54L15 DK.

## Features

- BLE Nordic UART Service (NUS) for bidirectional data streaming
- Real-time throughput measurement (TX/RX kbps)
- MIPS estimation during data transfer
- Optimized for maximum throughput (2M PHY, 244-byte packets)
- Performance statistics printed every second

## Building

```bash
west build -b nrf54l15dk/nrf54l15/cpuapp
```

## Flashing

```bash
west flash
```

## Testing

1. Flash the firmware to your nRF54L15 DK
2. Connect to the device using a BLE UART app (e.g., nRF Connect, nRF Toolbox)
3. Device will advertise as "nRF54L15_Test"
4. Once connected, the device automatically streams test data
5. Send data from your phone to test bidirectional throughput
6. Monitor serial output for performance statistics

## Serial Output

The device outputs performance stats every second:
- TX/RX throughput in kbps
- Total bytes transferred
- Average cycles per transfer
- Estimated MIPS

## Connection Parameters

- PHY: 2M for higher throughput
- MTU: 247 bytes
- Connection interval: 7.5ms (optimized for throughput)
- Data length extension enabled
