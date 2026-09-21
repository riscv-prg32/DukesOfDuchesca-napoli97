#!/usr/bin/env python3
"""Render audio.json to a stereo WAV for human listening review.

This is a from-scratch, close-but-not-bit-exact re-implementation of PRG32's
SID-like mixer (triangle/saw/pulse/noise oscillators, linear ADSR, pan law)
purely for previewing the composition on a host machine that has no PRG32
runtime -- it is not part of the cartridge build.
"""
import json
import math
import struct
import sys
import wave
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
SR = 22050
TEMPO_BPM = 150
LOOPS = 2


def midi_hz(note):
    return 440.0 * (2.0 ** ((note - 69) / 12.0))


def synth_wave(kind, phase, pulse_width_frac):
    # phase in [0,1)
    if kind == 0:  # triangle
        return 2.0 * abs(2.0 * (phase % 1.0) - 1.0) - 1.0
    if kind == 1:  # saw
        return 2.0 * (phase % 1.0) - 1.0
    if kind == 2:  # pulse
        return np.where((phase % 1.0) < pulse_width_frac, 1.0, -1.0)
    return None  # noise handled separately


def decode_synth(sample_id):
    wave_k = sample_id & 3
    pulse_width = (sample_id >> 2) & 0xF
    cutoff = (sample_id >> 6) & 0xF
    resonance = (sample_id >> 10) & 3
    return wave_k, pulse_width, cutoff, resonance


def adsr_env(t, dur, a, d, s, r):
    # a,d,s,r are the raw 0..255 bytes; map roughly like the real engine's
    # "quadratic ~1ms..~2s" note (approximate, for preview purposes only).
    def t_ms(v):
        return 1.0 + (v / 255.0) ** 2 * 2000.0

    a_s, d_s, r_s = t_ms(a) / 1000, t_ms(d) / 1000, t_ms(r) / 1000
    sustain_level = s / 255.0
    note_on_dur = dur
    total = note_on_dur + r_s
    env = np.zeros_like(t)
    for i, tt in enumerate(t):
        if tt < 0 or tt > total:
            continue
        if tt < a_s:
            env[i] = tt / a_s if a_s > 0 else 1.0
        elif tt < a_s + d_s:
            frac = (tt - a_s) / d_s if d_s > 0 else 1.0
            env[i] = 1.0 + (sustain_level - 1.0) * frac
        elif tt < note_on_dur:
            env[i] = sustain_level
        else:
            frac = (tt - note_on_dur) / r_s if r_s > 0 else 1.0
            env[i] = sustain_level * max(0.0, 1.0 - frac)
    return env


def main():
    cfg = json.loads((ROOT / "audio.json").read_text())
    instruments = cfg["instruments"]
    events = cfg["tracks"][0]["events"]

    ms_per_tick = 60000 / (TEMPO_BPM * 4)
    tick = 0
    notes = []  # (channel, start_s, dur_s, note)
    active = {}
    total_ticks = 0
    for e in events:
        tick += e["delta"]
        cmd = e["command"]
        if cmd == "SET_TEMPO":
            pass
        elif cmd == "NOTE_ON":
            ch = e["arg0"]
            active[ch] = (tick, e["arg1"])
        elif cmd == "NOTE_OFF":
            ch = e["arg0"]
            if ch in active:
                start, note = active.pop(ch)
                notes.append((ch, start, tick - start, note))
        elif cmd == "JUMP":
            total_ticks = tick

    loop_s = total_ticks * ms_per_tick / 1000
    total_s = loop_s * LOOPS + 1.0
    n = int(total_s * SR)
    stereo = np.zeros((n, 2), dtype=np.float64)

    rng = np.random.default_rng(1234)

    for loop_i in range(LOOPS):
        offset_s = loop_i * loop_s
        for ch, start_tick, dur_ticks, note in notes:
            inst = instruments[ch]
            sample_id = inst["sample_id"]
            wave_k, pw, cutoff, res = decode_synth(sample_id)
            start_s = offset_s + start_tick * ms_per_tick / 1000
            dur_s = dur_ticks * ms_per_tick / 1000
            a, d, s, r = inst["attack"], inst["decay"], inst["sustain"], inst["release"]
            tail_s = 1.0 + (r / 255.0) ** 2 * 2.0
            note_n = int((dur_s + tail_s) * SR)
            t = np.arange(note_n) / SR
            env = adsr_env(t, dur_s, a, d, s, r)
            freq = midi_hz(note)
            if wave_k == 3:
                sig = rng.uniform(-1, 1, size=note_n)
                # crude low-pass to emulate the cutoff/resonance filter
                alpha = 0.05 + cutoff / 15.0 * 0.5
                filt = np.zeros(note_n)
                acc = 0.0
                for i in range(note_n):
                    acc += alpha * (sig[i] - acc)
                    filt[i] = acc
                sig = filt / (np.max(np.abs(filt)) + 1e-9)
            else:
                phase = (t * freq) % 1.0
                pw_frac = (pw + 1) / 17.0
                sig = synth_wave(wave_k, phase, pw_frac)
            sig = sig * env * (inst["default_volume"] / 255.0)

            pan = inst["default_pan"]
            pan = max(-64, min(63, pan))
            left_gain = 1.0 if pan <= 0 else 1.0 - (pan / 63.0)
            right_gain = 1.0 if pan >= 0 else 1.0 - ((-pan) / 64.0)

            start_sample = int(start_s * SR)
            end_sample = start_sample + note_n
            if end_sample > n:
                sig = sig[: n - start_sample]
                end_sample = n
            if start_sample < 0 or start_sample >= n:
                continue
            stereo[start_sample:end_sample, 0] += sig * left_gain
            stereo[start_sample:end_sample, 1] += sig * right_gain

    peak = np.max(np.abs(stereo)) + 1e-9
    stereo = stereo / peak * 0.85
    pcm = (stereo * 32767).astype(np.int16)

    out = ROOT / "assets" / "theme_preview.wav"
    with wave.open(str(out), "wb") as wf:
        wf.setnchannels(2)
        wf.setsampwidth(2)
        wf.setframerate(SR)
        wf.writeframes(pcm.tobytes())
    print(f"wrote {out}: {total_s:.1f}s, {len(notes)} notes, loop={loop_s:.2f}s")


if __name__ == "__main__":
    main()
