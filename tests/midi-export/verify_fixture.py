"""Independently parse the native planner/writer regression fixture with Mido."""

from collections import defaultdict, deque
from importlib.metadata import version
from pathlib import Path
import sys

import mido


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def parse_track(track):
    absolute_tick = 0
    name = None
    notes = []
    active = defaultdict(deque)
    end_tick = None
    tempos = []
    meters = []

    for message in track:
        absolute_tick += message.time
        if message.type == "track_name":
            name = message.name
        elif message.type == "set_tempo":
            tempos.append((absolute_tick, message.tempo))
        elif message.type == "time_signature":
            meters.append((absolute_tick, message.numerator, message.denominator))
        elif message.type == "note_on" and message.velocity > 0:
            active[(message.channel, message.note)].append((absolute_tick, message.velocity))
        elif message.type == "note_off" or (message.type == "note_on" and message.velocity == 0):
            key = (message.channel, message.note)
            require(active[key], f"unmatched note-off: {name} channel={key[0]} note={key[1]}")
            start, velocity = active[key].popleft()
            notes.append((start, absolute_tick, message.note, velocity, message.channel))
        elif message.type == "end_of_track":
            end_tick = absolute_tick

    require(not any(active.values()), f"unclosed note-on events in track {name}")
    require(end_tick is not None, f"missing end-of-track in {name}")
    return name, sorted(notes), end_tick, tempos, meters


def main(path: Path) -> None:
    mido_version = version("mido")
    require(mido_version == "1.3.3", f"expected pinned Mido 1.3.3, got {mido_version}")
    midi = mido.MidiFile(path)
    require(midi.type == 1, f"expected format 1, got {midi.type}")
    require(midi.ticks_per_beat == 480, f"expected PPQ 480, got {midi.ticks_per_beat}")
    require(len(midi.tracks) == 4, f"expected 4 tracks, got {len(midi.tracks)}")

    parsed = [parse_track(track) for track in midi.tracks]
    names = [track[0] for track in parsed]
    require(names == ["Conductor", "Lead", "Bass", "Pattern 1 / Bell"], f"track order/names differ: {names}")

    conductor = parsed[0]
    require(conductor[3] == [(0, 500000)], f"unexpected tempo map: {conductor[3]}")
    require(conductor[4] == [(0, 3, 4)], f"unexpected meter map: {conductor[4]}")

    expected = {
        "Lead": [
            (1200, 1680, 60, 64, 0),
            (1680, 2160, 64, 64, 0),
            (1680, 2160, 67, 64, 0),
        ],
        "Bass": [(3840, 4800, 36, 127, 1)],
        "Pattern 1 / Bell": [
            (0, 240, 72, 64, 2),
            (960, 1200, 72, 64, 2),
            (1920, 2160, 72, 64, 2),
        ],
    }
    for name, notes, end_tick, _, _ in parsed[1:]:
        require(notes == expected[name], f"note events differ for {name}: {notes}")
        require(end_tick == 5760, f"{name} EOT should preserve arrangement silence through 5760, got {end_tick}")

    print(f"Mido {mido_version}: format 1, PPQ 480, {len(parsed)} tracks, {sum(len(t[1]) for t in parsed)} exact note pairs")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: verify_fixture.py path/to/midi-export-validation.mid")
    main(Path(sys.argv[1]))
