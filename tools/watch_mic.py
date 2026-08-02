"""
Live mic/beat-detector monitor for the desk pet.

Auto-finds the board's COM port (its CH343 USB-serial chip, VID 0x1A86),
resets it, turns on the firmware's 'm' mic-debug prints, and streams them
live until you hit Ctrl+C. Play music near the mic while this is running to
watch `music=yes/no` and `bpm=` react.

Usage:
    python watch_mic.py
    python watch_mic.py COM7      (skip auto-detect, use this port)
"""
import sys
import time
import serial
import serial.tools.list_ports

BAUD = 230400  # normal chat/debug baud - see NORMAL_BAUD in src/main.cpp
CH343_VID = 0x1A86


def find_port():
    for p in serial.tools.list_ports.comports():
        if p.vid == CH343_VID:
            return p.device
    return None


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    port_name = sys.argv[1] if len(sys.argv) > 1 else find_port()
    if not port_name:
        print("Couldn't find the board's COM port automatically.")
        print("Pass it explicitly, e.g.: python watch_mic.py COM7")
        sys.exit(1)

    print(f"Connecting to {port_name} at {BAUD} baud...")
    port = serial.Serial(port_name, BAUD, timeout=1)

    # Explicit reset pulse - a plain port-open doesn't reliably reset this
    # board's CH343 bridge chip.
    port.dtr = False
    port.rts = True
    time.sleep(0.1)
    port.rts = False
    time.sleep(2.5)  # let the boot banner finish
    port.reset_input_buffer()

    port.write(b"m\n")
    print("Mic debug on. Play music near the mic - watch for music=yes and a")
    print("steady bpm=. Press Ctrl+C to stop.\n")

    try:
        while True:
            line = port.readline()
            if line:
                text = line.decode(errors="replace").rstrip()
                if text:
                    print(text)
    except KeyboardInterrupt:
        pass
    finally:
        try:
            port.write(b"m\n")  # toggle debug back off
            time.sleep(0.2)
        except Exception:
            pass
        port.close()
        print("\nDisconnected.")


if __name__ == "__main__":
    main()
