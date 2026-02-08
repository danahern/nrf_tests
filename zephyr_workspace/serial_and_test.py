#!/usr/bin/env python3
"""Run serial monitor and L2CAP test concurrently."""
import serial
import subprocess
import threading
import time
import sys

port = "/dev/tty.usbmodem0010577098713"
baud = 115200

def serial_reader():
    """Read serial in background thread."""
    try:
        ser = serial.Serial(port, baud, timeout=1)
        print("[SERIAL] Connected")
        while True:
            line = ser.readline()
            if line:
                text = line.decode('utf-8', errors='replace').rstrip()
                print(f"[SERIAL] {text}")
    except Exception as e:
        print(f"[SERIAL] Error: {e}")

# Start serial reader
t = threading.Thread(target=serial_reader, daemon=True)
t.start()

# Reset device
print("[MAIN] Resetting device...")
subprocess.run(["nrfutil", "device", "reset", "--serial-number", "1057709871"],
               capture_output=True)
time.sleep(3)

# Run L2CAP test
print("[MAIN] Starting L2CAP test...")
proc = subprocess.Popen(
    ["/Users/danahern/.pyenv/versions/zephyr-env/bin/python3", "/Users/danahern/code/claude/embedded/zephyr_workspace/l2cap_throughput_test.py",
     "--duration", "30"],
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
)

for line in proc.stdout:
    print(f"[TEST] {line.rstrip()}")

proc.wait()
print("[MAIN] Test finished")
time.sleep(2)
