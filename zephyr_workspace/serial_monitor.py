#!/usr/bin/env python3
"""Read serial output from nRF54L15 DK. Resets the device first to capture boot logs."""
import serial
import subprocess
import sys
import time

port = "/dev/tty.usbmodem0010577098713"
baud = 115200

ser = serial.Serial(port, baud, timeout=1)
print(f"Listening on {port}...")
print("Resetting device via nrfutil...")

# Reset the device
subprocess.run(["nrfutil", "device", "reset", "--serial-number", "1057709871"],
               capture_output=True)
print("Device reset. Waiting for output (30s)...\n")

end_time = time.time() + 60
while time.time() < end_time:
    line = ser.readline()
    if line:
        print(line.decode('utf-8', errors='replace').rstrip())

ser.close()
print("\nDone.")
