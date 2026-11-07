import serial
import time
import sys

PORT = "COM11"
BAUD = 115200

print(f"Connecting to {PORT} at {BAUD} baud...")
try:
    ser = serial.Serial(PORT, BAUD, timeout=0.1)
    print(f"Monitor connected to {PORT}! Press Ctrl+C to stop.\n" + "="*50)
    while True:
        line = ser.readline()
        if line:
            text = line.decode('utf-8', errors='replace').rstrip()
            if text:
                print(text)
                sys.stdout.flush()
except KeyboardInterrupt:
    print("\nMonitor closed.")
except Exception as e:
    print(f"\nSerial error: {e}")
