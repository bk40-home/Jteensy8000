#!/usr/bin/env python3
# =============================================================================
# jp_wavemorph_gen.py — measured JP-8000 SHAPE morph tables from JE-8086 captures
# =============================================================================
#
# ROLE
#   Turns ONE capture WAV of a JE-8086 (JP-8000 DSP emulation) SHAPE sweep into
#   a morph wavetable header for the firmware: NFRAMES single cycles spanning
#   SHAPE min..max, each stored at several band-limited "mip" levels so the
#   oscillator can pick an alias-free level for the note being played.
#
# CAPTURE PROTOCOL (what the input WAV must contain — 4 held notes, ~8 s each,
# separated by silence, OSC1 only, filter fully open, no FX/mod, note C2):
#   note 1 : SHAPE at minimum (static)
#   note 2 : SHAPE at maximum (static)
#   note 3 : SHAPE swept by hand max -> min
#   note 4 : SHAPE swept by hand min -> max      <- the morph path is taken HERE
#   Note 3 is used only to cross-check note 4 (reports a consistency figure).
#
# CALIBRATED-RAMP MODE (--ramp T0 T1, added 2026-10-22)
#   For captures where SHAPE is driven by DAW automation as one LINEAR ramp
#   across a held note (e.g. the 6-pass Ctrl1/Ctrl2 sweep files).  The knob
#   law is then known, not inferred: knob(t) = (t − T0) / (T1 − T0), t from
#   note-on of the selected long note (--pass, 1-based), T0/T1 taken from an
#   independent calibration (2026-10-22: the square's PW ramp, linear to
#   ±0.002 duty: T0 = −1.318 s, T1 = 40.93 s).  Frame k is the average of
#   --avg-cycles phase-coherent cycles centred on knob k/(N−1); a knob value
#   outside the note is clamped to the note (frame 0 then sits at the knob
#   reached 0.1 s after note-on — reported).  --frames sets N (33 keeps
#   mid-point interpolation error ≤ 2 % for the JP triangle).  --f0 pins the
#   pitch.  Steps 4–6 below are shared with the hand-sweep mode.
#
# METHOD (each step is executed, not assumed — see the printed report)
#   1. f0: harmonic-comb search pinned near the stated note, then refined from
#      the phase slope of harmonic 1 across the static head/tail of note 4, so
#      every frame shares ONE continuous oscillator phase (no per-frame
#      alignment needed, no phasey crossfades between frames).
#   2. Motion window: each 50 ms frame's harmonic magnitudes are placed between
#      the static-min and static-max spectra; the sweep is where that position
#      leaves the end stops.
#   3. Frames: frame 0 / frame N-1 are long static averages (many cycles, low
#      noise); interior frames are 2-cycle averages at times spaced LINEARLY
#      across the motion window.  KNOB LAW CAVEAT: a hand sweep is not a
#      calibrated ramp, so frame index ~ knob position only approximately.
#   4. Output-coupling correction: the capture chain has a 1-pole high-pass
#      (estimated from a square capture, default 6.4 Hz).  Its tilt is removed
#      in the frequency domain: X(k) *= 1 + fc / (j k f0).
#   5. DC removed per frame; ONE gain for the whole set (peak -> INT16 full
#      scale x HEADROOM) so the level changes across SHAPE stay authentic.
#   6. Mips: level L keeps harmonics 1..H_L (zero above) and is resampled to
#      LEN_L = 4*H_L points (min 256), by inverse FFT — exactly band-limited.
#
# OUTPUT
#   A header "JpMorph_<name>.h" with one int16 array per wave (read through
#   core/dsp/WaveTableSet.h by OscCore's shared table reader):
#     [frame][concatenated mip levels], plus per-level offset/length/maxHarm
#   tables.  The firmware reader chooses the level whose maxHarm × f0 stays
#   under Nyquist.  Flash-only (PROGMEM via AkwfCompat.h, same as AKWF), zero RAM.
#
# USAGE
#   python tools/jp_wavemorph_gen.py vsaw.wav --name VSaw
#   python tools/jp_wavemorph_gen.py vtri.wav --name VTri
#   python tools/jp_wavemorph_gen.py tri_jp.wav --name VTri --ramp -1.318 40.93 \
#          --pass 1 --frames 33 --f0 130.8132 --hpf-hz 0     (2026-10-22 table)
#   (default --out is src/data/akwf/JpMorph — alongside the AKWF banks, in
#   flash via the same AkwfCompat PROGMEM plumbing, included ONLY by
#   WavetableLib.cpp, the wavetable firewall TU)
#
# © 2026 Kris Bishop — MIT licensed.
# =============================================================================
import argparse
import os
import numpy as np
from scipy.io import wavfile

# --- table geometry (kept small and explicit; the firmware mirrors these) ----
NFRAMES   = 17                      # hand-sweep default; --frames overrides
MIP_HARMS = [320, 160, 80, 40, 20, 10, 5]   # harmonics kept per level
MIN_LEN   = 256
HEADROOM  = 0.98                    # int16 peak = 0.98 × 32767
HOP_S     = 0.05                    # descriptor frame hop


def load(path):
    sr, x = wavfile.read(path)
    x = x.astype(np.float64)
    if x.ndim > 1:
        x = x.mean(axis=1)
    return sr, x / np.abs(x).max()


def cycles(x, sr, t, f0, n, K):
    """Average n cycles starting at time t, resampled to K points/cycle.
    Uses the TRUE oscillator phase grid (t, f0) so frames stay coherent."""
    P = sr / f0
    idx = np.arange(len(x))
    st = t * sr
    acc = np.zeros(K)
    for c in range(n):
        acc += np.interp(st + c * P + np.arange(K) * P / K, idx, x)
    return acc / n


def comb_f0(x, sr, t0, t1, guess):
    s = x[int(t0 * sr):int(t1 * sr)]
    N = len(s)
    S = np.abs(np.fft.rfft(s * np.hanning(N), 16 * N)) ** 2
    df = sr / (16 * N)
    best = (0.0, guess)
    for fc in guess * 2 ** (np.linspace(-60, 60, 481) / 1200):
        H = np.arange(1, int(10000 / fc))
        e = S[np.round(H * fc / df).astype(int)].sum()
        if e > best[0]:
            best = (e, fc)
    return best[1]


def phase_refine_f0(x, sr, f0, spans):
    """Refine f0 from harmonic-1 phase slope, fitted SEPARATELY inside each
    static span (the SHAPE is constant there, so harmonic-1 phase moves only
    with pitch).  One fit across the sweep would be biased: changing SHAPE
    rotates harmonic 1's phase too.  Slopes are length-weighted."""
    num = den = 0.0
    for a, b in spans:
        ts = np.arange(a, b - 0.2, 0.05)
        if len(ts) < 4:
            continue
        ph = []
        for t in ts:
            s = x[int(t * sr):int((t + 0.2) * sr)]
            n = np.arange(len(s)) / sr + t
            ph.append(np.angle(np.sum(s * np.exp(-2j * np.pi * f0 * n))))
        slope = np.polyfit(ts, np.unwrap(np.array(ph)), 1)[0]   # rad/s residual
        w = b - a
        num += slope * w
        den += w
    return f0 - (num / den) / (2 * np.pi) if den > 0 else f0


def note_windows(x, sr):
    env = np.convolve(np.abs(x), np.ones(2048) / 2048, 'same')
    on = (env > 0.03).astype(int)
    e = np.flatnonzero(np.diff(on))
    starts = [0] if on[0] else []
    starts += list(e[on[e + 1] == 1] + 1)
    ends = list(e[on[e + 1] == 0])
    if on[-1]:
        ends.append(len(x) - 1)
    wins = [(s / sr, t / sr) for s, t in zip(starts, ends) if (t - s) / sr > 2.0]
    if len(wins) < 4:
        raise SystemExit(f'expected 4 notes, found {len(wins)}: {wins}')
    return wins[:4]


def mags(F):
    C = np.abs(np.fft.rfft(F, axis=1))[:, 1:41]
    return C / C.max(axis=1, keepdims=True)


def emit(a, sr, f0, frames, nframes, note):
    """Steps 4-6 + header emit, shared by both modes.  `frames` is
    (nframes, K) phase-coherent single cycles at K points/cycle."""
    K = frames.shape[1]
    # 4./5. HPF undo, DC, single gain
    C = np.fft.rfft(frames, axis=1)
    k = np.arange(C.shape[1])
    H = np.ones(C.shape[1], complex)
    H[1:] = 1 + a.hpf_hz / (1j * k[1:] * f0)
    C *= H
    C[:, 0] = 0
    nyq_h = int((sr / 2) / f0)             # harmonics actually present
    C[:, nyq_h + 1:] = 0
    base = np.fft.irfft(C, K, axis=1)
    gain = HEADROOM / np.abs(base).max()
    C *= gain

    # 6. mips
    levels = []
    for Hm in MIP_HARMS:
        Hm = min(Hm, nyq_h)
        L = max(MIN_LEN, 4 * Hm)
        L = 1 << int(np.ceil(np.log2(L)))
        Cl = np.zeros((nframes, L // 2 + 1), complex)
        Cl[:, 1:Hm + 1] = C[:, 1:Hm + 1] * (L / K)
        lv = np.fft.irfft(Cl, L, axis=1)
        levels.append((Hm, L, np.clip(np.round(lv * 32767), -32767, 32767).astype(np.int16)))
    per_frame = sum(L for _, L, _ in levels)
    print(f'[{a.name}] levels (harm/len): ' + ', '.join(f'{h}/{L}' for h, L, _ in levels)
          + f'; {per_frame} samples/frame, {per_frame * nframes * 2 / 1024:.0f} KB flash')

    # emit
    os.makedirs(a.out, exist_ok=True)
    nm = a.name
    path = os.path.join(a.out, f'JpMorph_{nm}.h')
    with open(path, 'w', newline='\r\n') as f:
        f.write('// ' + '=' * 77 + '\n')
        f.write(f'// JpMorph_{nm}.h — GENERATED by tools/jp_wavemorph_gen.py. Do not edit.\n')
        f.write(f'// Source: {os.path.basename(a.wav)} (JE-8086 capture), f0 {f0:.4f} Hz,\n')
        f.write(f'// {note}, {nframes} frames x {len(levels)} mip levels.\n')
        f.write('// Layout: k{nm}[frame * kSamplesPerFrame + kLevelOffset[level] + i]\n'.replace('{nm}', nm))
        f.write('// ' + '=' * 77 + '\n#pragma once\n#include <stdint.h>\n#include "data/akwf/AkwfCompat.h"   // PROGMEM -> .progmem (flash), see its header\n\n')
        f.write(f'namespace JT {{ namespace JpMorph{nm} {{\n\n')
        f.write(f'inline constexpr uint16_t kFrames = {nframes};\n')
        f.write(f'inline constexpr uint16_t kLevels = {len(levels)};\n')
        f.write(f'inline constexpr uint32_t kSamplesPerFrame = {per_frame};\n')
        offs = np.cumsum([0] + [L for _, L, _ in levels[:-1]])
        f.write('inline constexpr uint16_t kLevelOffset[kLevels] = { ' + ', '.join(str(int(o)) for o in offs) + ' };\n')
        f.write('inline constexpr uint16_t kLevelLen[kLevels]    = { ' + ', '.join(str(L) for _, L, _ in levels) + ' };\n')
        f.write('inline constexpr uint16_t kLevelHarm[kLevels]   = { ' + ', '.join(str(h) for h, _, _ in levels) + ' };\n\n')
        f.write(f'static const int16_t kData[{per_frame * nframes}] PROGMEM = {{\n')
        for fr in range(nframes):
            f.write(f'    // frame {fr}\n')
            row = np.concatenate([lv[fr] for _, _, lv in levels])
            for i in range(0, len(row), 16):
                f.write('    ' + ', '.join(str(int(v)) for v in row[i:i + 16]) + ',\n')
        f.write('};\n\n} } // namespace JT::JpMorph' + nm + '\n')
    if a.dump_npy:
        np.save(os.path.join(a.out, f'JpMorph_{nm}_level0.npy'), levels[0][2])
    print(f'[{a.name}] wrote {path}')




def long_notes(x, sr, min_s=20.0):
    """Held notes longer than min_s seconds (the sweep passes), in order."""
    env = np.convolve(np.abs(x), np.ones(480) / 480, 'same')
    on = (env > 1e-3).astype(int)
    e = np.flatnonzero(np.diff(on))
    starts = ([0] if on[0] else []) + list(e[on[e + 1] == 1] + 1)
    ends = list(e[on[e + 1] == 0]) + ([len(x) - 1] if on[-1] else [])
    return [(s / sr, t / sr) for s, t in zip(starts, ends) if (t - s) / sr > min_s]


def main_ramp(a, sr, x):
    """Calibrated-ramp mode (see header): frames placed by KNOB, not time."""
    notes = long_notes(x, sr)
    if len(notes) < a.pass_no:
        raise SystemExit(f'pass {a.pass_no} requested, {len(notes)} long notes found')
    s, e = notes[a.pass_no - 1]
    f0 = a.f0 if a.f0 > 0 else comb_f0(x, sr, s + 0.1, s + 0.6, a.note_hz)
    T0, T1 = a.ramp
    K = 2048
    n = a.avg_cycles
    span = n / f0
    lo, hi = 0.1, (e - s) - 0.2 - span        # usable window, s from note-on
    frames = np.zeros((a.frames, K))
    t_ref = s + 0.1                           # common phase reference
    clamped = []
    for k in range(a.frames):
        u = k / (a.frames - 1)
        t = T0 + u * (T1 - T0) - span / 2     # window centred on knob u
        if t < lo or t > hi:
            clamped.append((k, u, (min(max(t, lo), hi) + span / 2 - T0) / (T1 - T0)))
        t = min(max(t, lo), hi)
        ts = t_ref + np.round((s + t - t_ref) * f0) / f0   # whole periods from t_ref
        frames[k] = cycles(x, sr, ts, f0, n, K)
    print(f'[{a.name}] ramp mode: pass {a.pass_no} note {s:.2f}-{e:.2f} s, f0 {f0:.4f} Hz, '
          f'{a.frames} frames x {n}-cycle averages')
    for k, u, ur in clamped:
        print(f'[{a.name}]   frame {k} (knob {u:.3f}) clamped to the note -> knob {ur:.3f}')
    emit(a, sr, f0, frames, a.frames,
         f'calibrated ramp T0 {T0} s, T1 {T1} s, pass {a.pass_no}, HPF undo {a.hpf_hz} Hz')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('wav')
    ap.add_argument('--name', required=True, help='C++ identifier stem, e.g. VSaw')
    ap.add_argument('--out', default=os.path.join('src', 'data', 'akwf', 'JpMorph'))
    ap.add_argument('--note-hz', type=float, default=65.406, help='captured note (C2)')
    ap.add_argument('--dump-npy', action='store_true', help='also save level-0 frames (.npy) for analysis')
    ap.add_argument('--hpf-hz', type=float, default=6.4, help='capture-chain HPF to undo')
    ap.add_argument('--ramp', type=float, nargs=2, metavar=('T0', 'T1'),
                    help='calibrated linear SHAPE ramp: knob 0 at T0, max at T1 (s from note-on)')
    ap.add_argument('--pass', dest='pass_no', type=int, default=1,
                    help='ramp mode: which long note (1-based) carries the ramp')
    ap.add_argument('--frames', type=int, default=NFRAMES, help='ramp mode: frame count')
    ap.add_argument('--avg-cycles', type=int, default=8, help='ramp mode: cycles averaged per frame')
    ap.add_argument('--f0', type=float, default=0.0, help='ramp mode: pinned pitch (Hz)')
    a = ap.parse_args()

    sr, x = load(a.wav)
    if a.ramp:
        main_ramp(a, sr, x)
        return
    w = note_windows(x, sr)
    print(f'[{a.name}] notes (s): ' + ', '.join(f'{s:.2f}-{e:.2f}' for s, e in w))
    s4, e4 = w[3]
    s3, e3 = w[2]

    # 1. pitch + phase-coherent grid for note 4
    f0 = comb_f0(x, sr, s4 + 0.1, s4 + 0.6, a.note_hz)
    K = 2048
    probe_t = np.arange(s4 + 0.1, e4 - 0.2, HOP_S)
    # static head/tail of note 4 found from the descriptor below; refine after
    def track(s, e):
        ts = np.arange(s + 0.1, e - 0.2, HOP_S)
        F = np.array([cycles(x, sr, t, f0, 2, 512) for t in ts])
        return ts, mags(F)
    ts4, M4 = track(s4, e4)
    mn, mx = M4[:4].mean(0), M4[-4:].mean(0)
    def pos(M):
        dA = np.linalg.norm(M - mn, axis=1)
        dB = np.linalg.norm(M - mx, axis=1)
        return dA / (dA + dB + 1e-12)
    p4 = pos(M4)
    mv = np.flatnonzero((p4 > 0.03) & (p4 < 0.97))
    t_on, t_off = ts4[mv[0]] - HOP_S, ts4[mv[-1]] + HOP_S
    f0 = phase_refine_f0(x, sr, f0, [(s4 + 0.1, t_on - 0.05), (t_off + 0.1, e4 - 0.2)])
    print(f'[{a.name}] f0 = {f0:.4f} Hz, sweep window (note 4) = {t_on - s4:.2f}..{t_off - s4:.2f} s')

    # 3. frames
    frames = np.zeros((NFRAMES, K))
    head_n = max(8, int((t_on - s4 - 0.15) * f0))
    tail_n = max(8, int((e4 - 0.2 - t_off - 0.1) * f0))
    # Every frame starts a WHOLE number of periods after one reference time,
    # so all frames share the oscillator's phase and crossfade cleanly.
    t_ref = s4 + 0.1
    def snap(t):
        return t_ref + np.round((t - t_ref) * f0) / f0
    frames[0] = cycles(x, sr, t_ref, f0, head_n, K)
    frames[-1] = cycles(x, sr, snap(t_off + 0.1), f0, tail_n, K)
    for k in range(1, NFRAMES - 1):
        t = t_on + (t_off - t_on) * k / (NFRAMES - 1)
        frames[k] = cycles(x, sr, snap(t - 1.0 / f0), f0, 2, K)   # centred on t

    # cross-check against note 3 (reversed, time-normalised)
    ts3, M3 = track(s3, e3)
    p3 = pos(M3)
    mv3 = np.flatnonzero((p3 > 0.03) & (p3 < 0.97))
    if len(mv3):
        a3, b3 = ts3[mv3[0]], ts3[mv3[-1]]
        errs = []
        for k in range(1, NFRAMES - 1):
            u = k / (NFRAMES - 1)
            t3 = b3 - (b3 - a3) * u
            m3 = mags(cycles(x, sr, t3, f0, 2, 512)[None, :])[0]
            m4 = mags(frames[k][None, ::4])[0]
            errs.append(np.linalg.norm(m3 - m4) / np.linalg.norm(m4))
        # PATH agreement: does each note-4 frame exist somewhere on note 3's
        # sweep (ignores hand speed)?  TIMING agreement (errs) assumes both
        # sweeps were equally paced — a low path / high timing figure means
        # the shapes are right and only the knob law is uncertain.
        path = []
        for k in range(1, NFRAMES - 1):
            m4 = mags(frames[k][None, ::4])[0]
            path.append(np.min(np.linalg.norm(M3[mv3] - m4, axis=1)) / np.linalg.norm(m4))
        print(f'[{a.name}] note 3 vs note 4 (rel. spectral diff) — path: median '
              f'{np.median(path):.2f}, worst {np.max(path):.2f}; timing: median '
              f'{np.median(errs):.2f}, worst {np.max(errs):.2f}')

    emit(a, sr, f0, frames, NFRAMES, f'capture HPF {a.hpf_hz} Hz undone')


if __name__ == '__main__':
    main()
