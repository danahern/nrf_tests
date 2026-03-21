FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    python3 \
    python3-pip \
    libusb-1.0-0 \
    usbutils \
    && rm -rf /var/lib/apt/lists/*

# Alif tools will be mounted at /tools
# Firmware binaries at /fw
# The SE-UART device at /dev/ttyUSB0 (passed via --device)

WORKDIR /tools
