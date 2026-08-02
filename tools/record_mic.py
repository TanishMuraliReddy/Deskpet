"""
Records 8 seconds of live mic audio from the desk pet and saves it as a WAV
file, so you can actually listen back to what the mic is capturing.

The firmware runs normal chat/debug traffic at NORMAL_BAUD (230400, so stock
serial monitor UIs' preset baud dropdowns can select it) but temporarily
bumps up to RECORDING_BAUD (921600) for the raw PCM stream itself, which
needs more real-time throughput than 230400 can carry - this script switches
its own baud rate to match at the same two points.

Usage:
    python record_mic.py
    python record_mic.py COM7 out.wav
"""
import sys
import time
import wave
import serial
import serial.tools.list_ports

NORMAL_BAUD = 230400
RECORDING_BAUD = 921600
CH343_VID = 0x1A86
SAMPLE_RATE = 16000
DURATION_MS = 8000
EXPECTED_BYTES = SAMPLE_RATE * DURATION_MS // 1000 * 2  # 16-bit mono


def find_port():
    for p in serial.tools.list_ports.comports():
        if p.vid == CH343_VID:
            return p.device
    return None


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    port_name = sys.argv[1] if len(sys.argv) > 1 else find_port()
    out_wav = sys.argv[2] if len(sys.argv) > 2 else "mic_capture.wav"
    if not port_name:
        print("Couldn't find the board's COM port automatically.")
        print("Pass it explicitly, e.g.: python record_mic.py COM7")
        sys.exit(1)

    port = serial.Serial(port_name, NORMAL_BAUD, timeout=2)
    port.dtr = False
    port.rts = True
    time.sleep(0.1)
    port.rts = False
    time.sleep(2.5)  # let the boot banner finish
    port.reset_input_buffer()

    print("Sending record command...")
    port.write(b"record\n")

    deadline = time.time() + 5
    found_start = False
    while time.time() < deadline:
        line = port.readline()
        if not line:
            continue
        text = line.decode(errors="replace").strip()
        if text:
            print(f"  < {text}")
        if text == "REC_START":
            found_start = True
            break

    if not found_start:
        print("Never saw REC_START - is the firmware flashed with the 'record' command?")
        sys.exit(1)

    # The firmware switches its own UART up to RECORDING_BAUD right after
    # REC_START - match it here before reading the raw PCM stream.
    port.baudrate = RECORDING_BAUD
    port.reset_input_buffer()

    print(f"Capturing {EXPECTED_BYTES} bytes of PCM audio...")
    pcm = b""
    deadline = time.time() + 10
    while len(pcm) < EXPECTED_BYTES and time.time() < deadline:
        chunk = port.read(EXPECTED_BYTES - len(pcm))
        pcm += chunk

    print(f"Got {len(pcm)} of {EXPECTED_BYTES} expected bytes")

    # ...and back down to NORMAL_BAUD once the firmware's done streaming.
    time.sleep(0.1)
    port.baudrate = NORMAL_BAUD
    time.sleep(0.1)
    port.reset_input_buffer()
    port.close()

    with wave.open(out_wav, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(pcm)

    print(f"Saved {out_wav}")


if __name__ == "__main__":
    main()
