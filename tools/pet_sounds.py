"""
Plays the desk pet's robot sounds on this PC's speakers.

The sounds are synthesized here exactly the way the firmware does it
(VoxieAudio::playRobotSound): the tone tables are read straight out of
lib/VoxieAudio/VoxieAudio.cpp, so editing a sound there changes it here too.
Uses Windows' built-in winsound - nothing to install.

    python pet_sounds.py            plays every sound once
    python pet_sounds.py cute shy   plays just those
"""
import io
import re
import sys
import threading
import wave
from pathlib import Path

import numpy as np

SAMPLE_RATE = 24000  # matches kSampleRate in VoxieAudio.cpp
SOURCE = Path(__file__).resolve().parent.parent / "lib" / "VoxieAudio" / "VoxieAudio.cpp"
# Firmware sound name (as in "[audio] playing <name>") -> tone table in the source.
TABLES = {"cute": "kCute", "yes": "kYes", "no": "kNo", "scared": "kScared", "shy": "kShy",
          "wakeup": "kWakeup", "sleepy": "kSleepy", "angry": "kAngry", "tone": "kTestTone"}


def load_tables(path=SOURCE):
    """{name: [(duration_ms, start_hz, end_hz, level, harmonic, wobble_hz), ...]}"""
    src = path.read_text(encoding="utf-8")
    tables = {}
    for name, body in re.findall(r"constexpr ToneSegment (k\w+)\[\] = \{(.*?)\};", src, re.S):
        tables[name] = [tuple(float(v.strip().rstrip("f")) for v in seg.split(","))
                        for seg in re.findall(r"\{([^{}]+)\}", body)]
    return {sound: tables[t] for sound, t in TABLES.items() if t in tables}


def synthesize(segments, pitch=1.0, level=1.0):
    """Same math as VoxieAudio::playRobotSound, as int16 mono at SAMPLE_RATE."""
    out = []
    for duration_ms, start_hz, end_hz, seg_level, harmonic, wobble_hz in segments:
        n = max(1, int(duration_ms * SAMPLE_RATE / 1000))
        progress = np.arange(n) / n
        envelope = np.minimum(1.0, np.minimum(progress * 18.0, (1.0 - progress) * 18.0))
        frequency = (start_hz + (end_hz - start_hz) * progress) * pitch
        phase = np.cumsum(2 * np.pi * frequency / SAMPLE_RATE)
        wobble = np.cumsum(np.full(n, 2 * np.pi * wobble_hz / SAMPLE_RATE))
        voice = np.sin(phase) + harmonic * np.sin(phase * 2.0)
        tremolo = 0.92 + 0.08 * np.sin(wobble)
        out.append(voice * tremolo * envelope * seg_level * level * 32760.0)
    out.append(np.zeros(int(0.02 * SAMPLE_RATE)))  # the firmware's 20 ms DMA tail
    return np.clip(np.concatenate(out), -32768, 32767).astype("<i2")


def tone(hz, ms, level):
    t = np.arange(int(ms * SAMPLE_RATE / 1000)) / SAMPLE_RATE
    return (np.sin(2 * np.pi * hz * t) * level * 32760.0).astype("<i2")


def to_wav(samples):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(samples.tobytes())
    return buf.getvalue()


class PcSpeaker:
    """Plays pet sounds on the PC, one at a time, without blocking the caller."""

    def __init__(self):
        self.tables = load_tables()
        self._lock = threading.Lock()

    def names(self):
        return list(self.tables)

    def samples_for(self, name, pitch=1.0, level=1.0):
        if name == "longtone":
            return tone(440.0, 3000, 0.4 * level)
        if name not in self.tables:
            return None
        # The firmware plays its test tone without the mood coloring.
        return synthesize(self.tables[name], 1.0 if name == "tone" else pitch, level)

    def play(self, name, pitch=1.0, level=1.0):
        """Returns False for a name this PC can't play (e.g. 'pintest')."""
        samples = self.samples_for(name, pitch, level)
        if samples is None:
            return False
        data = to_wav(samples)

        def worker():
            import winsound
            with self._lock:  # winsound can't play from memory asynchronously
                winsound.PlaySound(data, winsound.SND_MEMORY)
        threading.Thread(target=worker, daemon=True).start()
        return True


if __name__ == "__main__":
    import time
    speaker = PcSpeaker()
    for name in sys.argv[1:] or speaker.names():
        samples = speaker.samples_for(name)
        print(f"{name}: {len(samples) / SAMPLE_RATE * 1000:.0f} ms")
        speaker.play(name)
        time.sleep(len(samples) / SAMPLE_RATE + 0.6)
