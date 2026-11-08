#!/usr/bin/env python3
"""Serial monitor that survives ESP32-C3 resets / power cycles (native USB re-enumerates).
Usage: python3 tools/monitor.py [/dev/cu.usbmodemXXX]   — keys typed + Enter are sent (r, X, h)."""
import glob, sys, threading, time
import serial  # pip install pyserial

def find_port():
    if len(sys.argv) > 1:
        return sys.argv[1]
    ports = sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*") + glob.glob("/dev/cu.usbserial*") + glob.glob("/dev/ttyUSB*"))
    return ports[0] if ports else None

cur = None
def stdin_pump():
    for line in sys.stdin:
        if cur:
            try: cur.write(line.strip().encode())
            except Exception: pass
threading.Thread(target=stdin_pump, daemon=True).start()

while True:
    port = find_port()
    if not port:
        time.sleep(0.1); continue
    try:
        cur = serial.Serial(port, 115200, timeout=0.1)
        print(f"\033[2m--- connected {port} ---\033[0m", flush=True)
        while True:
            data = cur.read(512)
            if data:
                sys.stdout.write(data.decode(errors="replace")); sys.stdout.flush()
    except (serial.SerialException, OSError):
        cur = None
        print("\033[2m--- device gone (reset / power cycle) — waiting… ---\033[0m", flush=True)
        time.sleep(0.1)
    except KeyboardInterrupt:
        break
