"""
Plays 8 seconds of live mic audio from the desk pet through your PC speakers
in near real time (talk during the capture and you should hear yourself),
while also saving the same take as a WAV file for replay afterward.

Needs the `sounddevice` package: pip install sounddevice

See record_mic.py's docstring for why this switches baud rate mid-session.

Usage:
    python live_listen.py
    python live_listen.py COM7 out.wav
"""
import sys
import time
import wave
import serial
import serial.tools.list_ports
import sounddevice as sd

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
        print("Pass it explicitly, e.g.: python live_listen.py COM7")
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

    port.baudrate = RECORDING_BAUD
    port.reset_input_buffer()

    print()
    print(">>> LIVE - talk now, you should hear yourself through your speakers <<<")
    print()

    buf = bytearray()
    stream = sd.RawOutputStream(samplerate=SAMPLE_RATE, channels=1, dtype="int16")
    stream.start()

    chunk_size = 1024  # ~32ms of audio at 16kHz/16-bit
    deadline = time.time() + (DURATION_MS / 1000) + 3
    received = 0
    while received < EXPECTED_BYTES and time.time() < deadline:
        chunk = port.read(min(chunk_size, EXPECTED_BYTES - received))
        if not chunk:
            continue
        stream.write(chunk)
        buf.extend(chunk)
        received += len(chunk)

    stream.stop()
    stream.close()
    print(f"Captured {received} of {EXPECTED_BYTES} expected bytes")

    time.sleep(0.1)
    port.baudrate = NORMAL_BAUD
    time.sleep(0.1)
    port.reset_input_buffer()
    port.close()

    with wave.open(out_wav, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(bytes(buf))

    print(f"Saved {out_wav}")


if __name__ == "__main__":
    main()
