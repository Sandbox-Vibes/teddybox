#!/usr/bin/env python3
"""Checks the decoded test signal of taf_run.py (default chapters): chapter 1
440 Hz left / 660 Hz right, chapter 2 mono (left == right)."""
import sys, wave, numpy as np
w = wave.open(sys.argv[1]); x = np.frombuffer(w.readframes(w.getnframes()), '<i2').reshape(-1, 2) / 32768.0
a = x[4800:4800 + 96000]
def peak(c):
    s = np.abs(np.fft.rfft(c * np.hanning(len(c)))); return np.argmax(s) * 48000 / len(c)
fl, fr = peak(a[:, 0]), peak(a[:, 1])
rms = np.sqrt((a ** 2).mean(0))
m = x[3 * 48000 + 4800:3 * 48000 + 4800 + 14400]
mono = np.abs(m[:, 0] - m[:, 1]).max()
ok = abs(fl - 440) < 2 and abs(fr - 660) < 2 and all(0.3 < r < 0.4 for r in rms) and mono < 0.02
print(f"left {fl:.1f} Hz, right {fr:.1f} Hz, rms {rms[0]:.3f}/{rms[1]:.3f} (sine 0.5: 0.354), chapter 2 L-R max {mono:.4f}", 'OK' if ok else 'FAIL')
sys.exit(0 if ok else 1)
