"""
Mic bring-up terminal for the desk pet's `mic-test` firmware
(pio run -e mic-test -t upload).

Default: prints the firmware's live level meter, so you can confirm the
INMP441 hears you at all (bars should jump when you talk or clap).

--transcribe: switches the firmware into PCM streaming (works with both the
mic-test and the main firmware) and prints the words it hears, using Whisper on this PC (the ESP32 itself can't do speech
to text). Speech is cut into utterances by a simple loudness gate, so pause
briefly between sentences.
    pip install pyserial numpy faster-whisper

Usage:
    python mic_terminal.py
    python mic_terminal.py --transcribe
    python mic_terminal.py --transcribe COM7
    python mic_terminal.py --transcribe --model=small.en
For a live window with adjustable settings, see mic_display.py.
"""
import queue
import sys
import threading
import time
import serial
import serial.tools.list_ports

BAUD = 230400         # firmware text/command baud (main firmware and mic-test)
STREAM_BAUD = 921600  # what "stream on" switches both ends to
CH343_VID = 0x1A86
SAMPLE_RATE = 16000
CHUNK_SAMPLES = 800         # 50 ms of audio per gate decision
SPEECH_FACTOR = 2.0         # speech = this many times louder than the room's noise floor
MIN_SPEECH_RMS = 150        # 16-bit scale; never treat anything quieter as speech
END_SILENCE_CHUNKS = 14     # ~0.7 s of quiet ends an utterance
MAX_UTTERANCE_S = 15
DEFAULT_MODEL = "base.en"   # "tiny.en" is faster, "small.en" more accurate


def find_port():
    for p in serial.tools.list_ports.comports():
        if p.vid == CH343_VID:
            return p.device
    return None


def open_board(port_name):
    port = serial.Serial(port_name, BAUD, timeout=1)
    # Explicit reset pulse - a plain open doesn't reliably reset the CH343.
    port.dtr = False
    port.rts = True
    time.sleep(0.1)
    port.rts = False
    time.sleep(1.5)
    return port


def run_meter(port):
    print("Showing mic levels - talk or clap near the mic. Ctrl+C to stop.\n")
    while True:
        line = port.readline().decode(errors="replace").rstrip()
        if line:
            print(line)


def start_stream(port):
    """Asks the firmware to stream mic audio, then follows it to STREAM_BAUD."""
    port.reset_input_buffer()
    port.write(b"stream on\n")
    deadline = time.time() + 5
    while b"[stream] start" not in port.readline():
        if time.time() > deadline:
            sys.exit("Firmware never started streaming - flash the main or mic-test firmware.")
    port.baudrate = STREAM_BAUD


def read_stream(port, chunks, stop, on_text=None):
    """Splits the serial stream into audio and text.

    Audio arrives as packets: 0xA5 0x5A, uint16 LE sample count, int16 LE
    samples. Anything outside a packet is the firmware's normal text log,
    handed line by line to on_text. Audio goes into `chunks` re-cut to fixed
    CHUNK_SAMPLES pieces so the loudness gate sees even windows.

    Runs in its own thread so the serial port keeps draining while Whisper
    is busy; otherwise the OS buffer overflows and audio drops.
    """
    buf, audio, text = bytearray(), bytearray(), bytearray()
    chunk_bytes = CHUNK_SAMPLES * 2
    while not stop.is_set():
        buf += port.read(max(1, min(4096, port.in_waiting)))
        while True:
            sync = buf.find(b"\xa5\x5a")
            if sync < 0:
                # Keep a trailing 0xA5 in case it's the first half of a header.
                keep = 1 if buf.endswith(b"\xa5") else 0
                text += buf[:len(buf) - keep]
                del buf[:len(buf) - keep]
                break
            text += buf[:sync]
            if len(buf) < sync + 4:
                del buf[:sync]
                break
            count = buf[sync + 2] | (buf[sync + 3] << 8)
            if count == 0 or count > 2048:  # not a real header
                text += buf[sync:sync + 1]
                del buf[:sync + 1]
                continue
            end = sync + 4 + count * 2
            if len(buf) < end:
                del buf[:sync]
                break
            audio += buf[sync + 4:end]
            del buf[:end]
        while len(audio) >= chunk_bytes:
            chunks.put(bytes(audio[:chunk_bytes]))
            del audio[:chunk_bytes]
        while b"\n" in text:
            line, _, rest = bytes(text).partition(b"\n")
            text = bytearray(rest)
            if on_text:
                on_text(line.decode(errors="replace").strip())


def run_transcribe(port, model_name):
    import numpy as np
    from faster_whisper import WhisperModel

    print(f"Loading Whisper '{model_name}' (first run downloads it)...")
    model = WhisperModel(model_name, device="cpu", compute_type="int8")

    start_stream(port)
    print("Listening. Speak, then pause. Ctrl+C to stop.\n")

    chunks, stop = queue.Queue(), threading.Event()
    reader = threading.Thread(target=read_stream, args=(port, chunks, stop), daemon=True)
    reader.start()

    noise_floor = None
    utterance, silent_chunks, talking = [], 0, False
    try:
        while True:
            chunk = np.frombuffer(chunks.get(), dtype="<i2").astype(np.float32)
            chunk -= chunk.mean()  # drop the mic's small DC offset
            rms = float(np.sqrt(np.mean(chunk ** 2)))
            if noise_floor is None:
                noise_floor = rms
            threshold = max(MIN_SPEECH_RMS, noise_floor * SPEECH_FACTOR)
            if rms < threshold:
                # Only quiet chunks move the floor, so talking can't raise it.
                noise_floor = noise_floor * 0.98 + rms * 0.02

            bar = "#" * min(40, int(rms / 50))
            sys.stdout.write(f"\r  level {rms:6.0f} / {threshold:4.0f} |{bar:<40}|")
            sys.stdout.flush()

            if rms >= threshold:
                talking, silent_chunks = True, 0
            elif talking:
                silent_chunks += 1
            if talking:
                utterance.append(chunk)

            too_long = len(utterance) * CHUNK_SAMPLES >= MAX_UTTERANCE_S * SAMPLE_RATE
            if talking and (silent_chunks >= END_SILENCE_CHUNKS or too_long):
                audio = np.concatenate(utterance)
                # The mic is quiet at this gain; normalize so Whisper gets a usable level.
                audio /= max(1.0, float(np.max(np.abs(audio))))
                segments, _ = model.transcribe(audio, language="en", vad_filter=True)
                text = " ".join(s.text.strip() for s in segments).strip()
                sys.stdout.write("\r" + " " * 70 + "\r")
                print(f"heard: {text}" if text else "heard: (noise, no words)")
                utterance, silent_chunks, talking = [], 0, False
    finally:
        stop.set()
        reader.join(timeout=2)
        port.write(b"stream off\n")


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    port_name = args[0] if args else find_port()
    if not port_name:
        print("Couldn't find the board's COM port automatically.")
        print("Pass it explicitly, e.g.: python mic_terminal.py COM7")
        sys.exit(1)

    port = open_board(port_name)
    try:
        if "--transcribe" in sys.argv:
            model = next((a.split("=", 1)[1] for a in sys.argv if a.startswith("--model=")),
                         DEFAULT_MODEL)
            run_transcribe(port, model)
        else:
            run_meter(port)
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        port.close()


if __name__ == "__main__":
    main()
