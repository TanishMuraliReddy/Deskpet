"""
Live speech-to-text + voice-command window for the desk pet. Works with the
main firmware (commands are sent back so the pet actually reacts) or the
`mic-test` firmware (recognition only, nothing to react).

Nothing to tune - it adapts on its own:
  * Speech is found by a neural voice-activity detector (Silero VAD, bundled
    with faster-whisper) on auto-gained audio, not by a loudness cutoff, so
    quiet speech counts and fans/clicks/room noise don't.
  * It learns how loud *you* are (setup + every command it accepts) and how
    loud *other* talk is (TV, videos, people - speech with no "Voxie" in it).
    When both are known and clearly apart, a "Voxie" only counts if it's
    closer to your level than to the background's. Both keep updating.
  * Spellings Whisper uses for the name ("Foxy", "Boxy", ...) are matched
    and learned as they show up.

Commands work immediately: "Hey Voxie", "Voxie, wake up", or
"Voxie, go to sleep". Just "Voxie" arms it for a few seconds so a command
can follow on its own. Optional voice calibration is available in the window
if you need the speaker-level filter to distinguish you from background talk.
Each command goes to the board as a "voice ..." serial line (see
runVoiceCommand() in src/main.cpp). The Word log window shows every decision
and why.
    pip install pyserial numpy faster-whisper

Usage:
    python mic_display.py
    python mic_display.py COM7

The soundboard defaults to the pet's I2S amplifier. PC playback and mirroring
are optional settings in its sound window.
"""
import collections
import difflib
import queue
import re
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk

import numpy as np

from mic_terminal import SAMPLE_RATE, find_port, open_board, read_stream, start_stream
from pet_sounds import PcSpeaker

MODELS = ["tiny.en", "base.en", "small.en"]
MAX_UTTERANCE_S = 15
PARTIAL_EVERY_S = 0.8   # how often the in-progress text refreshes while talking

# Speech detection (VAD).
VAD_EVERY_CHUNKS = 5    # run the detector every 0.25 s
VAD_WINDOW_S = 12       # how much recent audio it looks at
VAD_END_SILENCE_MS = 600
VAD_PAD_MS = 200
AGC_TARGET = 0.3        # recent audio is scaled so its loud end sits here (VAD wants real levels)
AGC_MIN_REF = 100.0     # never boost near-silence beyond this (16-bit units)
MIN_CLIP_S = 0.45       # shorter than any word (incl. padding): a click or blip
METER_MIN, METER_MAX = 10.0, 3000.0  # level meter range (log scale)

# You-vs-background model.
YOU_HISTORY = 8           # recent accepted "you" levels kept
OTHER_HISTORY_S = 90      # other talk older than this is forgotten
OTHER_MIN_SAMPLES = 3     # need this many other-talk clips before judging by loudness
SEPARABLE_RATIO = 1.25    # you must be this much louder than the background to split by level
LONG_UTTERANCE_S = 6.0    # longer than any command: continuous talk is probably TV / video

WAKE_WORD = "voxie"
SEED_ALIASES = {"voxie", "voxy", "voxi", "voxey", "voxxie", "vox", "vexie",
                # This mic's soft, band-limited audio blurs V/F/B/R, so Whisper
                # routinely writes the name as one of these.
                "foxy", "foxie", "boxy", "boxie", "roxy", "roxie", "moxie", "proxy",
                # Common longer Whisper mishearings of "Hey Voxie" from this mic.
                "nevaise", "nevise", "novice"}
# Same idea for any other "-oxy"/"-oxie" spelling Whisper comes up with,
# including ones glued onto the word before it ("hayboxy" = "hey boxy"):
# group 1 is whatever got glued on, group 2 the name itself.
WAKE_PATTERN = re.compile(r"^([a-z]*?)((?:[bfv]a|[bfvpmrt]?r?o)x+(?:y|ie|i|ey|ee|e|he))$")
WAKE_FUZZY = 0.7        # also accept tokens this similar to "voxie" (or 0.8 to a known spelling)
HOTWORDS = "Hey Voxie, Voxie, Foxie, Boxy, Nevaise, Nevise, Novice, wake up, go to sleep, hello"
CALIBRATION_SAYS = 3
ARMED_WINDOW_S = 5.0    # after a bare "Voxie", how long a command alone still counts
COMMAND_SHOW_S = 2.5

# Command -> token phrases that trigger it, checked longest first.
COMMANDS = {
    "WAKE UP": [["wake", "up"], ["wakeup"], ["wake"], ["wakey"], ["awake"], ["get", "up"],
                ["rise", "and", "shine"], ["good", "morning"], ["get", "going"],
                ["turn", "on"], ["start", "up"]],
    "SLEEP": [["go", "to", "sleep"], ["sleep"], ["sleepy"], ["good", "night"], ["goodnight"],
              ["night", "night"], ["nap"], ["bedtime"], ["go", "to", "bed"], ["power", "down"],
              ["turn", "off"]],
    "HEY": [["hey"], ["hay"], ["yeah"], ["yes"], ["hi"], ["hiya"], ["heya"], ["hello"],
            ["howdy"], ["yo"], ["hey", "there"], ["hello", "there"], ["good", "evening"]],
}
COMMAND_FUZZY = 0.8     # single-word commands also match close misspellings ("sleap")
BOARD_COMMANDS = {"VOXIE": "voice voxie", "WAKE UP": "voice wake",
                  "SLEEP": "voice sleep", "HEY": "voice hey"}
# Board log lines worth showing; the rest (touch telemetry etc.) is noise here.
BOARD_LOG_PREFIXES = ("[voice]", "[event]", "[touch] TOUCHED", "[music] vibe",
                      "[audio] playing", "[audio] pintest", "[audio] mode=", "[audio] robot-sound volume",
                      "[audio] robot sound unavailable", "[audio] I2S",
                      "[mood] calling", "[mood] happy chirp")
# "[audio] playing cute 330ms": the pet is about to make a sound - the mic
# will hear it, so the voice window ignores that stretch (+ echo tail).
SELF_SOUND_LINE = re.compile(r"^\[audio\] playing (\S+) (\d+)ms")
SELF_SOUND_TAIL_MS = 300
COMMAND_COLORS = {"WAKE UP": "#2e7d32", "SLEEP": "#3949ab", "HEY": "#c2185b", "VOXIE": "#ef6c00"}
FILLER = {"hey", "hi", "hello", "the", "a", "uh", "um", "oh", "okay", "ok"}


def tokens_of(text):
    return re.sub(r"[^a-z' ]", " ", text.lower()).replace("'", "").split()


class CommandMatcher:
    """Finds the wake word and commands in a transcript, using learned spellings."""

    def __init__(self):
        self.aliases = set(SEED_ALIASES)
        self.just_learned = None  # set when find_wake() adopts a new spelling

    @staticmethod
    def _similar(a, b):
        return difflib.SequenceMatcher(None, a, b).ratio()

    def wake_parts(self, token, fuzzy=True):
        """(glued-on prefix, name) if token is the wake word, else None."""
        if token in self.aliases:
            return "", token
        # Whisper spells the "x" sound every way: "voksi", "rocksy".
        sounded = token.replace("cks", "x").replace("ks", "x")
        if sounded in self.aliases:
            return "", sounded
        m = WAKE_PATTERN.match(sounded)
        # A lone glued letter is more likely a real word ("epoxy") than "e-voxie".
        if m and len(sounded) <= 12 and len(m.group(1)) != 1:
            return m.group(1), m.group(2)
        if fuzzy and len(token) >= 4 and (
                self._similar(token, WAKE_WORD) >= WAKE_FUZZY or
                any(self._similar(token, a) >= 0.8 for a in self.aliases if len(a) >= 4)):
            return "", token
        return None

    @staticmethod
    def _split_glued(glued):
        """The command word at the end of a glued prefix: 'forhay' -> ['hay']."""
        words = sorted((p[0] for ps in COMMANDS.values() for p in ps if len(p) == 1),
                       key=len, reverse=True)
        return next(([w] for w in words if glued.endswith(w)), [])

    def add_alias(self, spelling):
        toks = tokens_of(spelling)
        if toks:
            self.aliases.add("".join(toks))

    def find_wake(self, toks):
        """(start, end, glued) for the wake word, also catching splits like 'vox e'.

        `glued` is any word Whisper fused onto the front of the name (the
        'hay' in 'hayboxy'), so the caller can still look for a command in it.
        New spellings are adopted as aliases, so the next time they match directly.
        """
        self.just_learned = None
        # Whole words first, so "for hayboxy" finds "hayboxy" rather than
        # gluing "for" on. Then two-word splits, but only the strict pattern
        # and only with a short tail ("box he") - fuzzy-matching pairs makes
        # ordinary phrases like "box is" count.
        candidates = [(i, i + 1, t, True) for i, t in enumerate(toks)]
        candidates += [(i, i + 2, toks[i] + toks[i + 1], False)
                       for i in range(len(toks) - 1) if len(toks[i + 1]) <= 2]
        for start, end, candidate, fuzzy in candidates:
            parts = self.wake_parts(candidate, fuzzy)
            if parts:
                glued, name = parts
                if name not in self.aliases:
                    self.aliases.add(name)
                    self.just_learned = name
                return start, end, self._split_glued(glued)
        return None

    def find_command(self, toks):
        for name, phrases in COMMANDS.items():
            for phrase in sorted(phrases, key=len, reverse=True):
                n = len(phrase)
                if any(toks[i:i + n] == phrase for i in range(len(toks) - n + 1)):
                    return name
        for name, phrases in COMMANDS.items():
            for phrase in phrases:
                if len(phrase) == 1 and len(phrase[0]) >= 4 and any(
                        self._similar(t, phrase[0]) >= COMMAND_FUZZY for t in toks):
                    return name
        return None

    def learn(self, text):
        """Calibration: remember how Whisper spelled 'Voxie'. Returns the spelling."""
        toks = [t for t in tokens_of(text) if t not in FILLER]
        # A whole sentence (background TV, talking to someone) isn't the name.
        if not toks or len(toks) > 3:
            return None
        known = [t for t in toks if t in self.aliases]
        if known:
            return known[0]
        similarity = lambda t: difflib.SequenceMatcher(None, t, WAKE_WORD).ratio()
        joined = "".join(toks)
        spelling = max(toks + [joined], key=similarity)
        # Must at least resemble it ("foxy" is ~0.44) and not be a tiny common word.
        if len(spelling) < 3 or similarity(spelling) < 0.35:
            return None
        self.aliases.add(spelling)
        return spelling


class Transcriber(threading.Thread):
    """Owns the Whisper model; takes ('partial'|'final', audio, peak_rms) jobs."""

    def __init__(self, ui, model_name):
        super().__init__(daemon=True)
        self.ui = ui
        self.jobs = queue.Queue()
        self.partial_pending = threading.Event()
        self.model = None
        self.load(model_name)

    def load(self, name):
        self.jobs.put(("load", name, None))

    def submit(self, kind, audio, peak_rms):
        if kind == "partial":
            # Drop partials while one is already queued; only the newest matters.
            if self.partial_pending.is_set():
                return
            self.partial_pending.set()
        self.jobs.put((kind, audio, peak_rms))

    def run(self):
        from faster_whisper import WhisperModel
        while True:
            kind, payload, peak_rms = self.jobs.get()
            if kind == "load":
                self.ui.put(("status", f"Loading Whisper '{payload}'..."))
                self.model = WhisperModel(payload, device="cpu", compute_type="int8")
                self.ui.put(("model_ready", payload))
                continue
            if kind == "partial":
                self.partial_pending.clear()
            if self.model is None:
                continue
            audio = payload / max(1.0, float(np.max(np.abs(payload))))  # quiet mic: normalize
            start = time.time()
            segments, _ = self.model.transcribe(audio, language="en", vad_filter=True,
                                                hotwords=HOTWORDS,
                                                beam_size=1 if kind == "partial" else 5)
            text = " ".join(s.text.strip() for s in segments).strip()
            self.ui.put((kind, text, time.time() - start, len(payload) / SAMPLE_RATE, peak_rms))


def speech_level(audio):
    """Loudness of a clip, pre-emphasized so hum/rumble doesn't count."""
    if len(audio) < 2:
        return 0.0
    emphasized = audio[1:] - 0.95 * audio[:-1]
    return float(np.sqrt(np.mean(emphasized ** 2)))


class Listener(threading.Thread):
    """Cuts the mic stream into utterances with a neural voice-activity detector."""

    def __init__(self, port, ui, settings, transcriber):
        super().__init__(daemon=True)
        self.port, self.ui, self.settings, self.transcriber = port, ui, settings, transcriber
        self.chunks, self.stop = queue.Queue(), threading.Event()

    def run(self):
        from faster_whisper.vad import VadOptions, get_speech_timestamps
        options = VadOptions(min_silence_duration_ms=VAD_END_SILENCE_MS, speech_pad_ms=VAD_PAD_MS)
        def on_text(line):
            # The pet announces each sound before playing it. That line sits
            # in the stream right before the audio of the sound itself, so a
            # marker in the chunk queue mutes exactly that stretch.
            m = SELF_SOUND_LINE.match(line)
            if m:
                self.chunks.put(("mute", int(m.group(2)) + SELF_SOUND_TAIL_MS))
            self.ui.put(("board", line))

        threading.Thread(target=read_stream, args=(self.port, self.chunks, self.stop, on_text),
                         daemon=True).start()

        keep = int(VAD_WINDOW_S * SAMPLE_RATE)
        ring = np.zeros(0, dtype=np.float32)
        total = 0           # samples received so far (absolute index of ring's end)
        done_until = 0      # absolute sample index everything before which is handled
        since_vad, last_partial, speaking = 0, 0.0, False
        end_gap = int(VAD_END_SILENCE_MS / 1000 * SAMPLE_RATE)
        mute_left = 0       # samples of the pet's own voice still to blank out

        while not self.stop.is_set():
            try:
                raw = self.chunks.get(timeout=0.5)
            except queue.Empty:
                continue
            if isinstance(raw, tuple):  # ("mute", ms) from on_text
                mute_left = max(mute_left, raw[1] * SAMPLE_RATE // 1000)
                continue
            chunk = np.frombuffer(raw, dtype="<i2").astype(np.float32)
            chunk -= chunk.mean()  # drop the mic's small DC offset
            if mute_left > 0:
                # Blank rather than drop, so the timeline stays intact and the
                # pet's loud chirp doesn't skew the auto-gain for the next 12 s.
                mute_left -= len(chunk)
                chunk[:] = 0.0
                done_until = total + len(chunk)
            ring = np.concatenate([ring, chunk])[-keep:]
            total += len(chunk)
            self.ui.put(("level", speech_level(chunk), speaking))

            if self.settings["paused"]:
                done_until, speaking = total, False
                continue
            since_vad += 1
            if since_vad < VAD_EVERY_CHUNKS:
                continue
            since_vad = 0

            ring_start = total - len(ring)
            start = max(done_until, ring_start)
            window = ring[start - ring_start:]
            if len(window) < SAMPLE_RATE // 2:
                continue
            # Auto-gain: the detector needs speech at a realistic level, and
            # this mic is quiet. Scale by the recent loud end of the audio.
            ref = max(AGC_MIN_REF, float(np.percentile(np.abs(ring), 99.9)))
            stamps = get_speech_timestamps(np.clip(window * (AGC_TARGET / ref), -1, 1), options)

            speaking = False
            for seg in stamps:
                seg_start, seg_end = start + seg["start"], start + seg["end"]
                audio = ring[seg_start - ring_start:seg_end - ring_start]
                length_s = len(audio) / SAMPLE_RATE
                finished = seg_end < total - end_gap
                if finished or length_s >= MAX_UTTERANCE_S:
                    if length_s >= MIN_CLIP_S:
                        self.transcriber.submit("final", audio, speech_level(audio))
                    done_until = seg_end
                    continue
                speaking = True
                if length_s > 0.5 and time.time() - last_partial >= PARTIAL_EVERY_S:
                    last_partial = time.time()
                    self.transcriber.submit("partial", audio, speech_level(audio))
            if not stamps:
                # Nothing speech-like: no need to re-scan this audio next time.
                done_until = max(done_until, total - end_gap)


class VoiceModel:
    """Learns your speaking level vs. everything else's, to tell you apart from a TV."""

    def __init__(self):
        self.you = collections.deque(maxlen=YOU_HISTORY)
        self.other = collections.deque()  # (time, level)

    def add_you(self, level):
        self.you.append(level)

    def add_other(self, level):
        self.other.append((time.time(), level))

    def _prune(self):
        cutoff = time.time() - OTHER_HISTORY_S
        while self.other and self.other[0][0] < cutoff:
            self.other.popleft()

    def you_level(self):
        return float(np.median(self.you)) if self.you else None

    def other_level(self):
        self._prune()
        if len(self.other) < OTHER_MIN_SAMPLES:
            return None
        return float(np.median([lv for _, lv in self.other]))

    def split(self):
        """Loudness that separates you from the background, or None if it can't."""
        you, other = self.you_level(), self.other_level()
        if you is None or other is None or you < other * SEPARABLE_RATIO:
            return None
        return float(np.sqrt(you * other))  # halfway in log terms

    def describe(self):
        you, other, split = self.you_level(), self.other_level(), self.split()
        parts = [f"your voice ~{you:.0f}" if you else "your voice: not learned yet",
                 f"other talk ~{other:.0f}" if other else "other talk: none lately"]
        if split:
            parts.append(f"split at {split:.0f}")
        elif you and other:
            parts.append("too close to split by loudness - going by words only")
        return "   ".join(parts)


class WordLog:
    """Pop-out window: one row per thing the mic registered, and why it did or didn't count."""

    COLUMNS = [("time", "Time", 64), ("level", "Level", 50), ("split", "Split", 50),
               ("length", "Len", 44), ("heard", "Whisper heard", 230), ("voxie", "Voxie as", 80),
               ("command", "Command", 76), ("result", "Result", 230)]

    def __init__(self, app):
        self.app = app
        self.win = tk.Toplevel(app.root)
        self.win.title("Word log - what Voxie registered")
        self.win.geometry("900x460")
        self.win.protocol("WM_DELETE_WINDOW", self.close)

        top = ttk.Frame(self.win, padding=(10, 8))
        top.pack(fill="x")
        self.levels = ttk.Label(top, font=("Consolas", 10))
        self.levels.pack(anchor="w")
        self.spellings = ttk.Label(top, wraplength=860)
        self.spellings.pack(anchor="w", pady=(4, 0))
        row = ttk.Frame(top)
        row.pack(anchor="w", pady=(6, 0))
        ttk.Label(row, text="Add a spelling for Voxie:").pack(side="left")
        self.alias_entry = ttk.Entry(row, width=14)
        self.alias_entry.pack(side="left", padx=(6, 0))
        self.alias_entry.bind("<Return>", lambda e: self.add_alias())
        ttk.Button(row, text="Add", command=self.add_alias, width=6).pack(side="left", padx=(4, 0))

        frame = ttk.Frame(self.win)
        frame.pack(fill="both", expand=True, padx=10, pady=(0, 10))
        self.tree = ttk.Treeview(frame, columns=[c for c, _, _ in self.COLUMNS], show="headings")
        for col, title, width in self.COLUMNS:
            self.tree.heading(col, text=title)
            self.tree.column(col, width=width, anchor="w", stretch=col in ("heard", "result"))
        scroll = ttk.Scrollbar(frame, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=scroll.set)
        self.tree.pack(side="left", fill="both", expand=True)
        scroll.pack(side="right", fill="y")
        self.tree.tag_configure("sent", foreground="#1b5e20")
        self.tree.tag_configure("ignored", foreground="#757575")
        self.tree.tag_configure("background", foreground="#6a1b9a")
        self.tree.tag_configure("calib", foreground="#455a64")
        for entry in app.word_log:
            self.add(entry)
        self.refresh()

    def add(self, e):
        split = f"{e['split']:.0f}" if e["split"] else "-"
        values = [e["time"], f"{e['level']:.0f}", split, f"{e['length']:.1f}s",
                  e.get("heard", ""), e.get("voxie", ""), e.get("command", ""), e["result"]]
        item = self.tree.insert("", "end", values=values, tags=(e["tag"],))
        self.tree.see(item)

    def refresh(self):
        self.levels.configure(text=self.app.voice.describe())
        self.spellings.configure(text="Counts as Voxie: " + ", ".join(sorted(self.app.matcher.aliases)))

    def add_alias(self):
        self.app.matcher.add_alias(self.alias_entry.get())
        self.alias_entry.delete(0, "end")
        self.refresh()

    def close(self):
        self.app.word_log_window = None
        self.win.destroy()


class SoundBoard:
    """Pop-out window: buttons that play each of the pet's sounds, plus speaker checks."""

    SOUNDS = [("Cute", "cute"), ("Yes", "yes"), ("No", "no"), ("Scared", "scared"),
              ("Shy", "shy"), ("Wake up", "wakeup"), ("Sleepy", "sleepy"), ("Angry", "angry")]
    CHECKS = [("Short tone", "tone"), ("Long tone (3 s)", "longtone"),
              ("Wire test (1-6 beeps)", "pintest"), ("Audio status", "status")]

    def __init__(self, app):
        self.app = app
        self.win = tk.Toplevel(app.root)
        self.win.title("Sounds - Voxie")
        self.win.resizable(False, False)
        self.win.protocol("WM_DELETE_WINDOW", self.close)
        where = ttk.Frame(self.win, padding=(12, 10, 12, 0))
        where.pack(fill="x")
        ttk.Label(where, text="Play on:", font=("Segoe UI", 10, "bold")).pack(side="left")
        self.target = tk.StringVar(value="pet")
        ttk.Radiobutton(where, text="PC speakers", value="pc", variable=self.target,
                        state="normal" if app.pc_speaker else "disabled").pack(side="left", padx=(8, 0))
        ttk.Radiobutton(where, text="Pet speaker", value="pet", variable=self.target).pack(side="left", padx=(8, 0))
        ttk.Checkbutton(where, text="Mirror everything the pet plays to PC", variable=app.mirror_var,
                        state="normal" if app.pc_speaker else "disabled").pack(side="left", padx=(16, 0))

        body = ttk.Frame(self.win, padding=12)
        body.pack(fill="both", expand=True)

        ttk.Label(body, text="Pet sounds", font=("Segoe UI", 10, "bold")).grid(row=0, column=0, columnspan=4, sticky="w")
        for i, (label, cmd) in enumerate(self.SOUNDS):
            ttk.Button(body, text=label, width=12, command=lambda c=cmd: self.send(c)).grid(
                row=1 + i // 4, column=i % 4, padx=3, pady=3)

        ttk.Label(body, text="Speaker checks", font=("Segoe UI", 10, "bold")).grid(
            row=3, column=0, columnspan=4, sticky="w", pady=(10, 0))
        for i, (label, cmd) in enumerate(self.CHECKS):
            ttk.Button(body, text=label, width=12 if i < 2 else 19, command=lambda c=cmd: self.send(c)).grid(
                row=4 + i // 2, column=(i % 2) * 2, columnspan=2, padx=3, pady=3, sticky="we")

        vol = ttk.Frame(body)
        vol.grid(row=6, column=0, columnspan=4, sticky="we", pady=(10, 0))
        ttk.Label(vol, text="Volume").pack(side="left")
        self.vol_var = tk.IntVar(value=100)
        self.vol_label = ttk.Label(vol, text="100%", width=5)
        scale = ttk.Scale(vol, from_=0, to=100, variable=self.vol_var, length=260,
                          command=lambda v: self.vol_label.configure(text=f"{float(v):.0f}%"))
        scale.pack(side="left", padx=8)
        self.vol_label.pack(side="left")
        # Send once the slider is let go, not on every step of the drag.
        scale.bind("<ButtonRelease-1>", lambda e: self.send(f"vol {self.vol_var.get()}"))

        ttk.Label(body, foreground="#666", wraplength=420, justify="left",
                  text="Hear nothing? Try Long tone, then Wire test: it plays 1 to 6 beeps, one count "
                       "per way the amp's BCLK/LRC/DIN wires could be arranged. The main window's log "
                       "shows which count used which pins.").grid(row=7, column=0, columnspan=4,
                                                                   sticky="w", pady=(10, 0))

    def send(self, cmd):
        if cmd.startswith("vol "):
            self.app.pc_volume = int(cmd.split()[1])  # one volume for both speakers
        elif self.target.get() == "pc" and self.app.play_on_pc(cmd):
            self.app.log(f"played on PC: {cmd}", info=True)
            return
        # Pet speaker (or something only the pet can do: wire test, status).
        # If mirroring is on, the pet's "[audio] playing" reply plays it on the PC too.
        self.app.port.write(f"audio {cmd}\n".encode())
        self.app.log(f"sent to pet: audio {cmd}", info=True)

    def close(self):
        self.app.sound_window = None
        self.win.destroy()


class App:
    def __init__(self, root, port):
        self.root, self.port = root, port
        self.ui = queue.Queue()
        self.settings = {"paused": False}
        self.matcher = CommandMatcher()
        self.voice = VoiceModel()
        self.word_log, self.word_log_window = [], None
        self.sound_window = None
        try:
            self.pc_speaker = PcSpeaker()
        except Exception as exc:  # e.g. run from outside the repo, firmware source not found
            print(f"PC speaker playback off: {exc}")
            self.pc_speaker = None
        self.mirror_var = tk.BooleanVar(value=False)
        self.pc_volume = 100
        self.model_ready = False
        self.mode = "run"              # commands work immediately; voice calibration is optional
        self.say_count, self.say_heard = 0, []
        self.armed_until = 0.0
        self.banner_hold_until = 0.0

        root.title("Desk Pet Mic - Voxie commands")
        root.geometry("820x600")
        root.minsize(560, 420)

        controls = ttk.Frame(root, padding=(10, 8))
        controls.pack(fill="x")
        ttk.Label(controls, text="Model").pack(side="left")
        self.model_var = tk.StringVar(value="base.en")
        model_box = ttk.Combobox(controls, textvariable=self.model_var, values=MODELS,
                                 state="readonly", width=9)
        model_box.pack(side="left", padx=(6, 16))
        model_box.bind("<<ComboboxSelected>>", lambda e: self.transcriber.load(self.model_var.get()))
        self.require_wake = tk.BooleanVar(value=False)
        ttk.Checkbutton(controls, text="Require \"Voxie\" first (optional)",
                        variable=self.require_wake).pack(side="left")
        self.send_to_board = tk.BooleanVar(value=True)
        ttk.Checkbutton(controls, text="Send to pet",
                        variable=self.send_to_board).pack(side="left", padx=(12, 0))
        ttk.Button(controls, text="🔊 Sounds", command=self.open_sounds,
                   width=10).pack(side="right", padx=(6, 0))
        ttk.Button(controls, text="Word log", command=self.open_word_log,
                   width=9).pack(side="right")
        self.calib_btn = ttk.Button(controls, text="Calibrate voice", command=self.calib_button, width=13)
        self.calib_btn.pack(side="right", padx=(0, 6))
        ttk.Button(controls, text="Clear", command=self.clear, width=7).pack(side="right", padx=(0, 6))
        self.pause_btn = ttk.Button(controls, text="Pause", command=self.toggle_pause, width=8)
        self.pause_btn.pack(side="right", padx=(0, 6))

        self.banner = tk.Frame(root, bg="#263238", height=120)
        self.banner.pack(fill="x", padx=10)
        self.banner.pack_propagate(False)
        self.banner_title = tk.Label(self.banner, font=("Segoe UI", 26, "bold"), fg="white", bg="#263238")
        self.banner_title.pack(pady=(14, 0))
        self.banner_sub = tk.Label(self.banner, font=("Segoe UI", 12), fg="#cfd8dc", bg="#263238")
        self.banner_sub.pack()

        # The pet's adaptive mood, from its "[mood] ..." reports (main firmware only).
        self.mood = tk.Canvas(root, height=34, bg="#fafafa", highlightthickness=0)
        self.mood.pack(fill="x", padx=10, pady=(8, 0))
        self.mood_values = None
        self.draw_mood()

        self.meter = tk.Canvas(root, height=26, bg="#1e1e1e", highlightthickness=0)
        self.meter.pack(fill="x", padx=10, pady=(8, 0))

        self.text = tk.Text(root, wrap="word", font=("Segoe UI", 13), padx=10, pady=8,
                            relief="flat", bg="#fafafa")
        self.text.pack(fill="both", expand=True, padx=10, pady=(8, 0))
        self.text.tag_configure("time", foreground="#888888", font=("Consolas", 10))
        self.text.tag_configure("final", foreground="#111111")
        self.text.tag_configure("partial", foreground="#9a9a9a", font=("Segoe UI", 13, "italic"))
        self.text.tag_configure("noise", foreground="#bbbbbb", font=("Segoe UI", 10))
        self.text.tag_configure("info", foreground="#607d8b", font=("Segoe UI", 10, "italic"))
        for name, color in COMMAND_COLORS.items():
            self.text.tag_configure(name, foreground=color, font=("Segoe UI", 13, "bold"))
        self.text.configure(state="disabled")

        self.status = tk.StringVar(value="Starting...")
        ttk.Label(root, textvariable=self.status, padding=(10, 4), foreground="#555").pack(fill="x")

        self.transcriber = Transcriber(self.ui, self.model_var.get())
        self.transcriber.start()
        self.listener = Listener(port, self.ui, self.settings, self.transcriber)
        self.listener.start()

        self.enter_run("Ready immediately; voice calibration is optional")
        self.open_word_log()
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(30, self.poll)

    # ---- widgets -------------------------------------------------------
    def show_banner(self, title, sub="", color="#263238", hold_s=0.0):
        for w in (self.banner, self.banner_title, self.banner_sub):
            w.configure(bg=color)
        self.banner_title.configure(text=title)
        self.banner_sub.configure(text=sub)
        self.banner_hold_until = time.time() + hold_s

    def toggle_pause(self):
        self.settings["paused"] = not self.settings["paused"]
        self.pause_btn.configure(text="Resume" if self.settings["paused"] else "Pause")
        self.set_partial("")

    def clear(self):
        self.text.configure(state="normal")
        self.text.delete("1.0", "end")
        self.text.configure(state="disabled")

    def _drop_partial(self):
        ranges = self.text.tag_ranges("partial")
        for start, end in reversed(list(zip(ranges[::2], ranges[1::2]))):
            self.text.delete(start, end)

    def set_partial(self, text):
        self.text.configure(state="normal")
        self._drop_partial()
        if text:
            self.text.insert("end-1c", text, "partial")
        self.text.configure(state="disabled")
        self.text.see("end")

    def log(self, text, command=None, info=False):
        self.text.configure(state="normal")
        self._drop_partial()
        self.text.insert("end-1c", time.strftime("%H:%M:%S  "), "time")
        if info:
            self.text.insert("end-1c", text, "info")
        elif text:
            self.text.insert("end-1c", text, "final")
        else:
            self.text.insert("end-1c", "(noise, no words)", "noise")
        if command:
            self.text.insert("end-1c", f"   → {command}", command)
        self.text.insert("end-1c", "\n")
        self.text.configure(state="disabled")
        self.text.see("end")

    MOOD_BARS = [("happy", "😊 happy", "#43a047"), ("energy", "⚡ energy", "#fbc02d"),
                 ("bond", "💞 bond", "#d81b60"), ("lonely", "🥺 lonely", "#5c6bc0"),
                 ("grump", "😠 grump", "#e53935"), ("annoy", "😤 annoyed", "#fb8c00")]

    def draw_mood(self):
        c = self.mood
        c.delete("all")
        if not self.mood_values:
            c.create_text(4, 17, anchor="w", fill="#888",
                          text="Mood: waiting for the pet (needs the main firmware)",
                          font=("Segoe UI", 10, "italic"))
            return
        w = max(1, c.winfo_width())
        slot = w / len(self.MOOD_BARS)
        for i, (key, label, color) in enumerate(self.MOOD_BARS):
            try:
                value = float(self.mood_values.get(key, 0))
            except ValueError:
                value = 0.0
            x0 = i * slot + 4
            bar_w = slot - 12
            c.create_text(x0, 9, anchor="w", text=f"{label} {value * 100:.0f}%", font=("Segoe UI", 9))
            c.create_rectangle(x0, 20, x0 + bar_w, 30, fill="#e0e0e0", width=0)
            c.create_rectangle(x0, 20, x0 + bar_w * max(0.0, min(1.0, value)), 30, fill=color, width=0)
            if key == "happy" and "base" in self.mood_values:
                # Where happiness settles - the learned baseline.
                bx = x0 + bar_w * max(0.0, min(1.0, float(self.mood_values["base"])))
                c.create_line(bx, 17, bx, 33, fill="#1b5e20", width=2)

    def draw_meter(self, level, speaking):
        c = self.meter
        w, h = max(1, c.winfo_width()), 26
        c.delete("all")
        span = np.log10(METER_MAX / METER_MIN)
        x = lambda v: int(w * min(1.0, max(0.0, np.log10(max(v, METER_MIN) / METER_MIN) / span)))
        c.create_rectangle(0, 0, x(level), h, fill="#4caf50" if speaking else "#5f7f9f", width=0)
        you, other, split = self.voice.you_level(), self.voice.other_level(), self.voice.split()
        if other:
            c.create_line(x(other), 0, x(other), h, fill="#ce93d8", width=2, dash=(4, 2))
        if you:
            c.create_line(x(you), 0, x(you), h, fill="#81c784", width=2, dash=(4, 2))
        if split:
            c.create_line(x(split), 0, x(split), h, fill="#ff9800", width=2)
        label = f"level {level:.0f}" + ("   ● speech" if speaking else "")
        if you:
            label += f"   you ~{you:.0f}"
        if other:
            label += f"   other talk ~{other:.0f}"
        c.create_text(8, h // 2, text=label, anchor="w", fill="white", font=("Consolas", 10))

    # ---- word log ------------------------------------------------------
    def play_on_pc(self, name):
        """Plays a pet sound on the PC, colored by the pet's current mood like the firmware does."""
        if not self.pc_speaker:
            return False
        try:
            energy = float(self.mood_values.get("energy", 1.0)) if self.mood_values else 1.0
            happy = float(self.mood_values.get("happy", 0.5)) if self.mood_values else 0.5
        except ValueError:
            energy, happy = 1.0, 0.5
        # Same formula as setMoodVoice() in src/main.cpp.
        pitch = min(1.3, max(0.7, 0.9 + 0.1 * energy + 0.1 * (happy - 0.5)))
        level = min(1.0, max(0.3, 0.55 + 0.45 * energy)) * self.pc_volume / 100
        return self.pc_speaker.play(name, pitch, level)

    def open_sounds(self):
        if self.sound_window:
            self.sound_window.win.lift()
        else:
            self.sound_window = SoundBoard(self)

    def open_word_log(self):
        if self.word_log_window:
            self.word_log_window.win.lift()
        else:
            self.word_log_window = WordLog(self)

    def record(self, tag, result, level, length, heard="", voxie="", command=""):
        entry = {"at": time.time(), "time": time.strftime("%H:%M:%S"), "tag": tag,
                 "result": result, "level": level, "split": self.voice.split(), "length": length,
                 "heard": heard, "voxie": voxie, "command": command}
        self.word_log.append(entry)
        if self.word_log_window:
            self.word_log_window.add(entry)
            self.word_log_window.refresh()

    # ---- setup ---------------------------------------------------------
    def calib_button(self):
        if self.mode == "run":
            self.start_calibration()
        else:
            self.log("Voice calibration skipped - commands remain enabled.", info=True)
            self.enter_run()

    def start_calibration(self):
        self.mode = "say"
        self.say_count, self.say_heard = 0, []
        self.calib_btn.configure(text="Skip setup")
        self.prompt_say()

    def prompt_say(self, note=""):
        sub = note or ("Waiting for the speech model to load..." if not self.model_ready
                       else "Speak normally, about as far away as you'll usually be.")
        self.show_banner(f'Say "VOXIE"   ({self.say_count + 1} of {CALIBRATION_SAYS})', sub, "#ef6c00")

    def calibration_utterance(self, text, level, length):
        spelling = self.matcher.learn(text) if text else None
        if not spelling:
            self.log(text, info=not text)
            self.record("calib", "setup: didn't sound like Voxie, asked again", level, length, heard=text)
            self.prompt_say("Didn't catch it - say \"Voxie\" again.")
            return
        self.say_count += 1
        self.say_heard.append(spelling)
        self.voice.add_you(level)
        self.log(f'heard "{text}"  (spelling: {spelling}, level {level:.0f})', info=True)
        self.record("calib", f"setup {self.say_count}/{CALIBRATION_SAYS}: learned your voice",
                    level, length, heard=text, voxie=spelling)
        if self.say_count < CALIBRATION_SAYS:
            self.prompt_say()
            return
        spellings = ", ".join(sorted(set(self.say_heard)))
        self.log(f"Setup done: your voice ~{self.voice.you_level():.0f}. \"Voxie\" heard as: {spellings}",
                 info=True)
        self.enter_run(f"Heard you as: {spellings}")

    def enter_run(self, note=""):
        self.mode = "run"
        self.calib_btn.configure(text="Calibrate voice")
        self.show_banner("Ready", note or 'Say "Voxie" + wake up / sleep / hey', "#2e7d32", hold_s=2.5)

    # ---- command mode --------------------------------------------------
    def handle_command_text(self, text, level, length):
        toks = tokens_of(text)
        now = time.time()
        wake = self.matcher.find_wake(toks)
        voxie = " ".join(toks[wake[0]:wake[1]]) if wake else ""
        if self.matcher.just_learned:
            self.log(f'New spelling of Voxie learned: "{self.matcher.just_learned}"', info=True)
        split = self.voice.split()
        quieter_than_split = split is not None and level < split

        command, why, tag = None, "", "ignored"
        if wake and quieter_than_split:
            # Said the name, but at the background's loudness, not yours.
            why = (f"ignored: sounded like the background (level {level:.0f}, "
                   f"needs {split:.0f}+)")
            tag = "background"
        elif wake:
            start, end, glued = wake
            command = self.matcher.find_command(toks[:start] + glued + toks[end:])
            if command:
                self.armed_until = 0.0
            elif length >= LONG_UTTERANCE_S:
                # A "boxy" somewhere in a long stretch of dialogue isn't you
                # calling the pet; only a full name + command counts in there.
                why = "ignored: name inside long continuous speech, likely TV / video"
                tag = "background"
            else:
                command = "VOXIE"
                self.armed_until = now + ARMED_WINDOW_S
                why = "name only - armed 5 s for a command"
        elif now < self.armed_until or not self.require_wake.get():
            armed = now < self.armed_until
            command = self.matcher.find_command(toks)
            if command:
                self.armed_until = 0.0
                why = "Voxie said just before" if armed else "Voxie not required"
            else:
                why = "no command word recognized"
        else:
            unarmed = self.matcher.find_command(toks)
            why = (f"ignored: heard '{unarmed.lower()}' but no Voxie first" if unarmed
                   else "other talk - learned as background")
            if length >= LONG_UTTERANCE_S:
                why = "long continuous speech - learned as background (TV / video?)"

        self.log(text, command)
        if not text:
            self.record("ignored", "no words (VAD heard a voice, Whisper didn't)", level, length)
            return
        if command:
            # Any accepted command is you: keep your level current.
            self.voice.add_you(level)
            sent = self.send_to_board.get()
            if sent:
                self.port.write((BOARD_COMMANDS[command] + "\n").encode())
            result = f"sent '{BOARD_COMMANDS[command]}'" if sent else "matched (sending off)"
            self.record("sent", result + (f" - {why}" if why else ""), level, length,
                        heard=text, voxie=voxie, command=command)
        else:
            if not wake:
                # Speech with no "Voxie" in it is the best sample of everything
                # that isn't you talking to the pet: TV, videos, other people.
                self.voice.add_other(level)
            self.record(tag, why, level, length, heard=text, voxie=voxie)

        if command == "VOXIE":
            self.show_banner("VOXIE!", "Listening for a command: wake up / sleep / hey",
                             COMMAND_COLORS["VOXIE"], hold_s=ARMED_WINDOW_S)
        elif command:
            icons = {"WAKE UP": "☀", "SLEEP": "☾", "HEY": "👋"}
            self.show_banner(f"{icons[command]}  {command}", f'heard: "{text}"',
                             COMMAND_COLORS[command], hold_s=COMMAND_SHOW_S)

    def idle_banner(self):
        if self.mode != "run" or time.time() < self.banner_hold_until:
            return
        if self.settings["paused"]:
            self.show_banner("Paused", "Press Resume to keep listening", "#546e7a")
        elif not self.require_wake.get():
            self.show_banner("Listening", "Say wake up / sleep / hey (Voxie optional)")
        else:
            self.show_banner('Say "Voxie"', "then: wake up / sleep / hey")

    # ---- event loop ----------------------------------------------------
    def poll(self):
        latest_level = None
        try:
            while True:
                msg = self.ui.get_nowait()
                kind = msg[0]
                if kind == "level":
                    latest_level = msg[1:]
                elif kind == "status":
                    self.status.set(msg[1])
                elif kind == "board":
                    if msg[1].startswith("[mood] happy="):
                        self.mood_values = dict(kv.split("=", 1) for kv in msg[1].split()[1:] if "=" in kv)
                        self.draw_mood()
                    elif (m := SELF_SOUND_LINE.match(msg[1])) and self.mirror_var.get():
                        self.play_on_pc(m.group(1))
                        self.log(f"pet: {msg[1]}  (also on PC)", info=True)
                    elif msg[1].startswith(BOARD_LOG_PREFIXES):
                        self.log(f"pet: {msg[1]}", info=True)
                elif kind == "model_ready":
                    self.model_ready = True
                    self.status.set(f"Listening - model {msg[1]}")
                    if self.mode == "say":
                        self.prompt_say()
                elif kind == "partial" and not self.settings["paused"]:
                    self.set_partial(msg[1] + " ...")
                elif kind == "final":
                    _, text, took, length, level = msg
                    if self.mode == "say":
                        self.calibration_utterance(text, level, length)
                    elif self.mode == "run":
                        self.handle_command_text(text, level, length)
                    self.status.set(f"Listening - model {self.model_var.get()} - "
                                    f"last {length:.1f}s clip took {took:.1f}s")
        except queue.Empty:
            pass
        self.idle_banner()
        if latest_level:
            self.draw_meter(*latest_level)
        self.root.after(30, self.poll)

    def close(self):
        self.listener.stop.set()
        try:
            self.port.write(b"stream off\n")
            self.port.close()
        except Exception:
            pass
        self.root.destroy()


def main():
    port_name = sys.argv[1] if len(sys.argv) > 1 else find_port()
    if not port_name:
        sys.exit("Couldn't find the board's COM port. Pass it, e.g.: python mic_display.py COM7")
    port = open_board(port_name)
    start_stream(port)

    root = tk.Tk()
    App(root, port)
    root.mainloop()


if __name__ == "__main__":
    main()
