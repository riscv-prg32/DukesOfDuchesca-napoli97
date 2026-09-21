#!/usr/bin/env python3
"""Compose audio.json: an original "Napoli '97" theme for the cartridge.

Not a transcription of any existing recording -- an original tune merely
inspired by the requested mashup: a driving tambourine/mandola tarantella
groove (fast 6/8, Andalusian-cadence-flavoured minor mode, very Neapolitan)
fused with a galloping arpeggiated "banjo-picking" country-chase feel (the
Dukes of Hazzard vibe) and a sustained synth pad under a 90s dance-style
chord loop (Am-G-F-E, one of the most common 90s Eurodance progressions --
very fitting for a 1997 Napoli setting).

Uses PRG32's SID-like procedural synth voices: 6 melodic/rhythmic channels
(0-5) for the music, leaving channels 6-7 free for the game's own sound
effects, per docs/software/audio_polyphony.md's channel-partitioning advice.
Output feeds tools/prg32audio_pack.py, which is pure Python and needs no
RISC-V toolchain, so this can be regenerated and packed entirely on host.
"""
import json
from pathlib import Path

OUT = Path(__file__).resolve().parents[1] / "audio.json"

# ---- SID-like synth id bit-packing (mirrors prg32_audio.h exactly) -------
WAVE_TRIANGLE, WAVE_SAW, WAVE_PULSE, WAVE_NOISE = 0, 1, 2, 3
SYNTH_MARKER = 0x8000


def synth_id(wave, pulse_width, cutoff, resonance):
    return (SYNTH_MARKER | ((resonance & 3) << 10) | ((cutoff & 0xF) << 6) |
            ((pulse_width & 0xF) << 2) | (wave & 3))


TICKS_PER_MEASURE = 12  # 6/8 at a dotted-quarter pulse of 6 sixteenth-ticks
TEMPO_BPM = 150  # tick = 60000/(150*4) = 100ms exactly

# ---- instruments: one fixed voice per channel, SID-voice style -----------
# channel : (name, synth, volume, pan, adsr)
INSTRUMENTS = [
    # 0 lead: bright pulse "banjo/mandolin" pluck, center-left
    dict(name="lead", synth=synth_id(WAVE_PULSE, 5, 10, 1),
        default_volume=220, default_pan=-15, attack=0, decay=10, sustain=120, release=18),
    # 1 harmony/counter-melody: saw, center-right, answers the lead
    dict(name="harmony", synth=synth_id(WAVE_SAW, 8, 8, 1),
        default_volume=160, default_pan=25, attack=0, decay=14, sustain=100, release=22),
    # 2 bass: triangle, center, galloping root-note pulse
    dict(name="bass", synth=synth_id(WAVE_TRIANGLE, 8, 9, 1),
        default_volume=210, default_pan=0, attack=0, decay=6, sustain=200, release=12),
    # 3 tambourine: noise, wide stereo, relentless tarantella 8th-note pulse
    dict(name="tambourine", synth=synth_id(WAVE_NOISE, 8, 13, 3),
        default_volume=90, default_pan=-45, attack=0, decay=4, sustain=0, release=6),
    # 4 mandola pluck: narrow-pulse arpeggio, opposite side from the tambourine
    dict(name="mandola", synth=synth_id(WAVE_PULSE, 2, 11, 2),
        default_volume=130, default_pan=45, attack=0, decay=8, sustain=40, release=10),
    # 5 pad: soft saw, wide right, 90s sustained chord bed (enters halfway)
    dict(name="pad", synth=synth_id(WAVE_SAW, 8, 5, 0),
        default_volume=95, default_pan=35, attack=40, decay=30, sustain=150, release=60),
    # 6-7 reserved for in-game SFX (see game.c) -- still declared here so the
    # single AUDIO block covers all 8 voices with sensible centered defaults.
    dict(name="sfx_a", synth=synth_id(WAVE_PULSE, 8, 12, 1),
        default_volume=200, default_pan=-10, attack=0, decay=6, sustain=0, release=8),
    dict(name="sfx_b", synth=synth_id(WAVE_NOISE, 8, 10, 2),
        default_volume=200, default_pan=10, attack=0, decay=10, sustain=0, release=14),
]

LEAD, HARM, BASS, TAMB, MAND, PAD = 0, 1, 2, 3, 4, 5

# ---- Andalusian-cadence-flavoured minor progression: Am-G-F-E ------------
# (i-VII-VI-V -- extremely common in both Mediterranean/tarantella folk and
# 90s Eurodance; E major's G# acts as the leading tone pulling back to Am.)
CHORDS = {
    "Am": dict(root=45, third=48, fifth=52, lead=[64, 69, 72, 71]),  # A2 C3 E3 | E4 A4 C5 B4
    "G":  dict(root=43, third=47, fifth=50, lead=[62, 67, 71, 69]),  # G2 B2 D3 | D4 G4 B4 A4
    "F":  dict(root=41, third=45, fifth=48, lead=[60, 65, 69, 67]),  # F2 A2 C3 | C4 F4 A4 G4
    "E":  dict(root=40, third=44, fifth=47, lead=[59, 64, 68, 64]),  # E2 G#2 B2 | B3 E4 G#4 E4
}
PROGRESSION = ["Am", "G", "F", "E"]


def notes_for_measure(measure_idx, chord_name, section_b):
    """Yield (channel, start_tick, dur_ticks, note) for one 12-tick measure."""
    m0 = measure_idx * TICKS_PER_MEASURE
    chord = CHORDS[chord_name]
    events = []

    # Bass: galloping root-fifth-root-root pulse on the dotted-quarter beats.
    events.append((BASS, m0 + 0, 3, chord["root"]))
    events.append((BASS, m0 + 3, 3, chord["fifth"] - 12))
    events.append((BASS, m0 + 6, 3, chord["root"]))
    events.append((BASS, m0 + 9, 3, chord["root"] + 12 if not section_b else chord["fifth"] - 12))

    # Tambourine: every eighth note (2 ticks) -- the relentless tarantella pulse.
    for t in range(0, TICKS_PER_MEASURE, 2):
        events.append((TAMB, m0 + t, 1, 60))

    # Mandola: arpeggiated pluck on the off-beats, outlining the triad.
    arp = [chord["third"] + 12, chord["fifth"] + 12, chord["root"] + 24, chord["fifth"] + 12]
    for i, t in enumerate((1, 4, 7, 10)):
        events.append((MAND, m0 + t, 2, arp[i]))

    # Lead: the original riff for this chord (4 notes per measure).
    lead_notes = chord["lead"]
    lead_ticks = (0, 3, 6, 9)
    for i, t in enumerate(lead_ticks):
        dur = 3 if i < 3 else 3
        note = lead_notes[i]
        if section_b:
            note += 12  # final-chorus lift: lead doubles up an octave
        events.append((LEAD, m0 + t, dur, note))

    if section_b:
        # Harmony answers a third below the lead, offset by half a beat.
        for i, t in enumerate(lead_ticks):
            events.append((HARM, m0 + t, 3, lead_notes[i] - 3))
        # Pad: one sustained chord tone across the whole measure.
        events.append((PAD, m0, TICKS_PER_MEASURE, chord["root"] + 24))
    elif measure_idx % 2 == 1:
        # Section A: a sparser call-and-response lick every other measure.
        events.append((HARM, m0 + 9, 3, chord["fifth"] + 12))

    return events


def build_events():
    all_events = []
    measure_idx = 0
    for section_b in (False, True):
        for chord_name in PROGRESSION:
            all_events.extend(notes_for_measure(measure_idx, chord_name, section_b))
            measure_idx += 1
    total_ticks = measure_idx * TICKS_PER_MEASURE

    # Expand (channel, start, dur, note) into discrete ON/OFF points, then
    # merge everything into one chronological stream (delta-encoded), the
    # way the tracker actually consumes it -- see cartridges/blackjack/audio.json.
    # Tie-break at equal ticks: TEMPO first, then any OFF (release before the
    # next attack), then ON.
    rank = {"TEMPO": 0, "OFF": 1, "ON": 2}
    points = [(0, "TEMPO", None, TEMPO_BPM)]
    for ch, start, dur, note in all_events:
        points.append((start, "ON", ch, note))
        points.append((start + dur, "OFF", ch, 0))
    points.sort(key=lambda p: (p[0], rank[p[1]]))

    events_json = []
    prev_tick = 0
    for tick, kind, ch, note in points:
        delta = tick - prev_tick
        prev_tick = tick
        if kind == "TEMPO":
            events_json.append({"delta": delta, "command": "SET_TEMPO", "arg0": note})
        elif kind == "ON":
            events_json.append({"delta": delta, "command": "NOTE_ON", "arg0": ch, "arg1": note})
        else:
            events_json.append({"delta": delta, "command": "NOTE_OFF", "arg0": ch})

    # Loop back to the start (index 0, right after which SET_TEMPO harmlessly
    # re-applies the same tempo -- same idiom as cartridges/blackjack/audio.json).
    delta_to_loop = total_ticks - prev_tick
    events_json.append({"delta": max(delta_to_loop, 0), "command": "JUMP", "arg0": 0, "arg1": 0})
    return events_json, total_ticks


def main():
    events, total_ticks = build_events()
    instruments_json = [
        {
            "sample_id": inst["synth"],
            "default_volume": inst["default_volume"],
            "default_pan": inst["default_pan"],
            "attack": inst["attack"],
            "decay": inst["decay"],
            "sustain": inst["sustain"],
            "release": inst["release"],
        }
        for inst in INSTRUMENTS
    ]
    config = {
        "instruments": instruments_json,
        "tracks": [{"name": "napoli97_theme", "events": events}],
    }
    OUT.write_text(json.dumps(config, indent=2) + "\n")
    ms_per_tick = 60000 / (TEMPO_BPM * 4)
    loop_seconds = total_ticks * ms_per_tick / 1000
    print(f"wrote {OUT}: {len(events)} events, {total_ticks} ticks, "
         f"{loop_seconds:.1f}s loop @ {TEMPO_BPM}bpm")


if __name__ == "__main__":
    main()
