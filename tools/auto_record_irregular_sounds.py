"""Automatically save an 8-second WAV after the pet detects an irregular sound.

Run this while the board is connected:
    python auto_record_irregular_sounds.py

It owns the serial port, prints the ordinary firmware event log, and writes
captures such as irregular_20260802_142530.wav in the current directory.
The recording begins just after the trigger, which is useful for repeated or
continuing noises and for checking whether the pet is hearing its own speaker.
"""
import sys
import time
import wave
from datetime import datetime

import serial
import serial.tools.list_ports

NORMAL_BAUD = 230400
RECORDING_BAUD = 921600
SAMPLE_RATE = 16000
CAPTURE_BYTES = SAMPLE_RATE * 8 * 2
CH343_VID = 0x1A86


def find_port():
    for port in serial.tools.list_ports.comports():
        if port.vid == CH343_VID:
            return port.device
    return None


def record(port):
    port.write(b"record\n")
    deadline = time.time() + 3
    while time.time() < deadline:
        if port.readline().decode(errors="replace").strip() == "REC_START":
            break
    else:
        print("Recorder did not start.")
        return

    port.baudrate = RECORDING_BAUD
    data = bytearray()
    deadline = time.time() + 11
    while len(data) < CAPTURE_BYTES and time.time() < deadline:
        data.extend(port.read(CAPTURE_BYTES - len(data)))
    port.baudrate = NORMAL_BAUD
    time.sleep(0.2)
    port.reset_input_buffer()

    filename = "irregular_" + datetime.now().strftime("%Y%m%d_%H%M%S") + ".wav"
    with wave.open(filename, "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(SAMPLE_RATE)
        out.writeframes(data)
    print(f"Saved {len(data)} bytes to {filename}")


def main():
    port_name = sys.argv[1] if len(sys.argv) > 1 else find_port()
    if not port_name:
        raise SystemExit("Board not found. Pass the COM port, e.g. COM6.")
    port = serial.Serial(port_name, NORMAL_BAUD, timeout=1)
    print(f"Listening on {port_name}. Press Ctrl+C to stop.")
    try:
        while True:
            line = port.readline().decode(errors="replace").strip()
            if not line:
                continue
            print(line)
            if line.startswith("[event] irregular sound"):
                record(port)
    finally:
        port.close()


if __name__ == "__main__":
    main()
