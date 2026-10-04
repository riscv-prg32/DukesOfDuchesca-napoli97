#!/usr/bin/env python3
"""Compose audio.json: the original soundtrack and effect voices of the cartridge.

Four tracks, none a transcription of an existing recording:

  0  title   a slow serenade over a habanera bass
  1  drive   the "Napoli '97" tarantella in 6/8: tambourine, galloping bass,
             a mandolin roll; the game speeds it up when someone is chasing
  2  party   a four-on-the-floor 90s dance loop for the villa
  3  busted  a short lament, played once

How the firmware's tracker reads it (docs/tools/audio.md, PRG32 main):

- `delta` is the wait AFTER an event, in ticks of a 16th note. Events with
  delta 0 run in the same tick, so they form chords.
- NOTE_ON plays instrument N on channel N at the channel's volume, and the
  mixer applies that volume twice (as the note's and as the channel's), so
  the loudness of a voice is (volume/256)^2. Instrument default volumes are
  not used by the tracker: every track therefore opens with SET_VOLUME and
  SET_PAN for its five channels.
- Synth ids use the documented layout: bits 11:10 resonance, 9:6 cutoff,
  5:2 pulse width, 1:0 waveform.

Channels 0-4 belong to the music. Channels 5-7 and instruments 5-10 are the
game's own voices (engine, horn, siren, pick-ups, crashes): see game.c.

    python3 scripts/gen_music.py          # write audio.json
    python3 scripts/gen_music.py --check  # fail if audio.json is out of date
"""
import json
import sys
from pathlib import Path

OUT = Path(__file__).resolve().parents[1] / "audio.json"

TRI, SAW, PULSE, NOISE = 0, 1, 2, 3


def synth(wave, pulse_width=8, cutoff=15, resonance=0):
    assert 0 <= pulse_width < 16 and 0 <= cutoff < 16 and 0 <= resonance < 4
    return 0x8000 | (resonance << 10) | (cutoff << 6) | (pulse_width << 2) | wave


def inst(name, sample_id, a, d, s, r, volume=200, pan=0):
    return dict(name=name, sample_id=sample_id, default_volume=volume, default_pan=pan,
                attack=a, decay=d, sustain=s, release=r)


LEAD, HARM, BASS, DRUM, ARP = range(5)
INSTRUMENTS = [
    inst("lead", synth(PULSE, 6, 12, 1), 0, 60, 150, 40),         # mandolin-like pluck
    inst("harmony", synth(SAW, 8, 9, 1), 4, 70, 120, 50),
    inst("bass", synth(TRI, 8, 8, 0), 0, 40, 200, 30),
    inst("drums", synth(NOISE, 8, 14, 0), 0, 35, 0, 20),          # low note: thump; high: tambourine
    inst("roll", synth(PULSE, 3, 13, 2), 0, 45, 40, 30),          # narrow-pulse banjo/mandola roll
    inst("engine", synth(SAW, 8, 4, 2), 10, 0, 255, 30),          # 5: held, re-pitched with speed
    inst("blip", synth(PULSE, 8, 14, 0), 0, 30, 80, 25),          # 6: fuel pump, warnings
    inst("crash", synth(NOISE, 8, 7, 1), 0, 70, 0, 40),           # 7: hits and scrapes
    inst("horn", synth(PULSE, 7, 10, 2), 5, 0, 255, 30),          # 8
    inst("siren", synth(TRI, 8, 12, 0), 10, 0, 255, 40),          # 9
    inst("chime", synth(TRI, 8, 15, 0), 0, 80, 0, 80),            # 10: pick-ups
]
MUSIC_CHANNELS = 5
#            lead harm bass drum roll
VOLUMES = [228, 172, 236, 140, 160]
PANS = [-12, 24, 0, -40, 40]

# Chords: bass root, then the triad an octave above it.
CHORDS = {
    "Am": (45, (57, 60, 64)), "G": (43, (55, 59, 62)), "F": (41, (53, 57, 60)),
    "E": (40, (52, 56, 59)), "C": (48, (60, 64, 67)), "Dm": (38, (50, 53, 57)),
}
KICK, TAMB, HAT = 36, 84, 96


def serenade():
    """Track 0: 8 bars of 4/4 at 92 bpm."""
    bars = [
        ("Am", [(0, 6, 76), (6, 2, 74), (8, 4, 72), (12, 4, 69)]),
        ("Am", [(0, 4, 71), (4, 4, 72), (8, 8, 76)]),
        ("Dm", [(0, 6, 77), (6, 2, 76), (8, 4, 74), (12, 4, 69)]),
        ("Am", [(0, 4, 72), (4, 4, 71), (8, 8, 69)]),
        ("E", [(0, 6, 68), (6, 2, 69), (8, 4, 71), (12, 4, 74)]),
        ("Am", [(0, 4, 72), (4, 4, 76), (8, 8, 81)]),
        ("Dm", [(0, 4, 77), (4, 4, 74), (8, 4, 76), (12, 4, 68)]),
        ("Am", [(0, 12, 69)]),
    ]
    notes = []
    for bar, (chord, melody) in enumerate(bars):
        t0 = bar * 16
        root, triad = CHORDS[chord]
        for start, dur, note in melody:
            notes.append((LEAD, t0 + start, dur, note))
        for half in (0, 8):  # the habanera: long, short, two even
            notes += [(BASS, t0 + half, 3, root), (BASS, t0 + half + 3, 1, root + 7),
                      (BASS, t0 + half + 4, 2, root + 12), (BASS, t0 + half + 6, 2, root + 7)]
        for i in range(8):   # mandolin in broken chords
            notes.append((ARP, t0 + i * 2, 2, triad[(i + bar) % 3] + 12))
        notes.append((HARM, t0, 14, triad[1]))
    return 92, notes, len(bars) * 16, True


def tarantella():
    """Track 1: 20 bars of 6/8 (12 ticks each) at 150 bpm."""
    a1 = [("Am", [76, 81, 76, 72, 76, 69]), ("G", [74, 79, 74, 71, 74, 67]),
          ("F", [72, 77, 72, 69, 72, 65]), ("E", [71, 76, 68, 71, 76, 76])]
    a2 = [("Am", [81, 80, 81, 84, 83, 81]), ("G", [79, 78, 79, 83, 81, 79]),
          ("F", [77, 76, 77, 81, 79, 77]), ("E", [76, 75, 76, 80, 83, 76])]
    b = [("C", [79, 76, 72, 76, 79, 84]), ("G", [83, 79, 74, 79, 83, 86]),
         ("Am", [84, 81, 76, 81, 84, 88]), ("E", [83, 80, 76, 80, 83, 76])]
    sections = [(a1, 0), (a2, 1), (b, 1), (b, 2), (a2, 2)]
    notes, bar = [], 0
    for section, fill in sections:
        for chord, melody in section:
            t0 = bar * 12
            root, triad = CHORDS[chord]
            for i, note in enumerate(melody):
                notes.append((LEAD, t0 + i * 2, 2, note))
            # Galloping bass on the two beats of the bar, with a pick-up.
            notes += [(BASS, t0, 4, root), (BASS, t0 + 4, 2, root + 12),
                      (BASS, t0 + 6, 4, root + 7), (BASS, t0 + 10, 2, root + 12)]
            # Tambourine: thump on the beat, jingles on the other eighths.
            for i in range(6):
                notes.append((DRUM, t0 + i * 2, 1, KICK if i % 3 == 0 else TAMB))
            if fill >= 1:  # the roll fills the gaps between the melody notes
                for i in range(6):
                    notes.append((ARP, t0 + i * 2 + 1, 1, triad[i % 3] + 12))
            if fill >= 2:  # a second voice a third (or so) under the tune
                notes.append((HARM, t0, 5, triad[1] + 12))
                notes.append((HARM, t0 + 6, 5, triad[2] + 12))
            bar += 1
    return 150, notes, bar * 12, True


def party():
    """Track 2: 8 bars of 4/4 at 132 bpm."""
    notes = []
    for bar, chord in enumerate(["Am", "F", "C", "G"] * 2):
        t0 = bar * 16
        root, triad = CHORDS[chord]
        for beat in range(4):
            notes.append((DRUM, t0 + beat * 4, 1, KICK))
            notes.append((DRUM, t0 + beat * 4 + 2, 1, HAT))
            notes.append((BASS, t0 + beat * 4 + 2, 2, root + 12))
        for i in range(16):
            notes.append((ARP, t0 + i, 1, triad[(i * 2) % 3] + 12 + (12 if i % 4 == 3 else 0)))
        riff = [(0, 3, triad[2] + 12), (3, 3, triad[0] + 24), (6, 2, triad[1] + 24),
                (8, 3, triad[0] + 24), (11, 3, triad[2] + 12), (14, 2, triad[1] + 12)]
        if bar >= 4:
            for start, dur, note in riff:
                notes.append((LEAD, t0 + start, dur, note))
        notes.append((HARM, t0, 15, triad[0] + 12))
    return 132, notes, 8 * 16, True


def busted():
    """Track 3: a short descending lament, then silence."""
    notes = [(LEAD, 0, 4, 69), (LEAD, 4, 4, 67), (LEAD, 8, 4, 65), (LEAD, 12, 10, 64),
             (HARM, 0, 4, 60), (HARM, 4, 4, 59), (HARM, 8, 4, 57), (HARM, 12, 10, 56),
             (BASS, 0, 12, 45), (BASS, 12, 10, 40), (BASS, 24, 12, 33), (LEAD, 24, 12, 57),
             (DRUM, 24, 2, KICK)]
    return 100, notes, 40, False


def compile_track(tempo, notes, length, loop):
    """(channel, start, duration, note) -> tracker events with delta-after timing."""
    events = [dict(command="SET_TEMPO", arg0=tempo)]
    for ch in range(MUSIC_CHANNELS):
        events.append(dict(command="SET_VOLUME", arg0=ch, arg1=VOLUMES[ch]))
        events.append(dict(command="SET_PAN", arg0=ch, arg1=PANS[ch] & 0xFF))
    loop_target = len(events)

    points = []  # (tick, order, event); at one tick: releases, then attacks
    for ch, start, dur, note in notes:
        assert 0 <= ch < MUSIC_CHANNELS and 0 < note < 128 and dur > 0
        assert start + dur <= length, "a note runs past the end of the track"
        points.append((start, 1, dict(command="NOTE_ON", arg0=ch, arg1=note)))
        points.append((start + dur, 0, dict(command="NOTE_OFF", arg0=ch)))
    points.sort(key=lambda p: (p[0], p[1], p[2]["arg0"]))
    # A release in the very tick the same channel attacks again is redundant.
    attacks = {(t, e["arg0"]) for t, o, e in points if o == 1}
    points = [p for p in points if p[1] == 1 or (p[0], p[2]["arg0"]) not in attacks]

    assert points[0][0] == 0, "the track must start on tick 0"
    for i, (tick, _, event) in enumerate(points):
        nxt = points[i + 1][0] if i + 1 < len(points) else length
        event["delta"] = nxt - tick
        assert 0 <= event["delta"] <= 255
        events.append(event)
    events.append(dict(command="JUMP", arg0=loop_target & 0xFF, arg1=loop_target >> 8) if loop
                  else dict(command="END"))
    for event in events:
        event.setdefault("delta", 0)
    assert sum(e["delta"] for e in events[loop_target:]) == length
    return events


def build():
    tracks = []
    for name, compose in (("title", serenade), ("drive", tarantella), ("party", party), ("busted", busted)):
        tempo, notes, length, loop = compose()
        events = compile_track(tempo, notes, length, loop)
        seconds = length * 60.0 / (tempo * 4)
        tracks.append((dict(name=name, events=events), seconds))
    config = dict(instruments=INSTRUMENTS, tracks=[t for t, _ in tracks])
    # One event per line keeps diffs readable without tripling the file size.
    lines = ['{"instruments": [']
    lines += ["  " + json.dumps(i) + ("," if n + 1 < len(INSTRUMENTS) else "") for n, i in enumerate(INSTRUMENTS)]
    lines.append('], "tracks": [')
    for n, track in enumerate(config["tracks"]):
        lines.append('  {"name": %s, "events": [' % json.dumps(track["name"]))
        ev = track["events"]
        lines += ["    " + json.dumps(e, sort_keys=True) + ("," if k + 1 < len(ev) else "") for k, e in enumerate(ev)]
        lines.append("  ]}" + ("," if n + 1 < len(config["tracks"]) else ""))
    lines.append("]}")
    text = "\n".join(lines) + "\n"
    assert json.loads(text) == config
    return text, tracks


def main():
    text, tracks = build()
    if "--check" in sys.argv:
        if OUT.read_text() != text:
            sys.exit("audio.json is out of date: run python3 scripts/gen_music.py")
        print("audio.json is up to date")
        return
    OUT.write_text(text)
    for track, seconds in tracks:
        print(f"track {track['name']}: {len(track['events'])} events, {seconds:.1f} s")
    print(f"wrote {OUT}: {len(INSTRUMENTS)} instruments, {len(tracks)} tracks, "
          f"{sum(len(t['events']) for t, _ in tracks) * 4} bytes of events")


if __name__ == "__main__":
    main()
