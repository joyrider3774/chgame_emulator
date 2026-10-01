"""Fits the emulator's piezo filter to a recording of a real CHGame.

    python tools/fit_piezo.py recording.wav [emulator.wav]

The recording is the device playing tests/sketches/chg_sfxtest (CHChess's
capture effect every 1.5 s), picked up by a microphone. emulator.wav is the
same program from the emulator with the filter off:

    chg_headless roms/sfxtest_capture.bin 14 NUL --wav emu.wav --no-piezo

(made here when not given). The response is the ratio of the two average
burst spectra, background noise taken off: what the piezo (plus the room
and the microphone) does to the pin signal. It is smoothed to sixth octaves,
held falling below 450 Hz and above 16 kHz where the bursts carry little to
measure, made minimum phase and cut to a 256-tap FIR at 48 kHz:
src/piezo_filter.c.
"""
import os
import subprocess
import sys
import wave

import numpy as np

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RATE = 48000
TAPS = 256
NF = 8192


def load(path):
    w = wave.open(path)
    r, sw, ch = w.getframerate(), w.getsampwidth(), w.getnchannels()
    a = np.frombuffer(w.readframes(w.getnframes()), dtype={1: np.uint8, 2: np.int16, 4: np.int32}[sw]).astype(float)
    return r, a.reshape(-1, ch).mean(axis=1)


def burst_starts(a, r):
    env = np.convolve(np.abs(a - a.mean()), np.ones(r // 100) / (r // 100), "same")
    on = np.where(env > env.max() * 0.25)[0]
    out, s, p = [], on[0], on[0]
    for i in on[1:]:
        if i - p > r * 0.3:
            out.append(s)
            s = i
        p = i
    out.append(s)
    return out


def avg_spectrum(a, r, starts, length):
    total, n = 0, 0
    for s in starts:
        s = max(0, s - int(0.02 * r))
        seg = a[s:s + length]
        if len(seg) < length:
            continue
        total = total + np.abs(np.fft.rfft((seg - seg.mean()) * np.hanning(length), n=NF)) ** 2
        n += 1
    return total / n, np.fft.rfftfreq(NF, 1 / r)


def main():
    rec_path = sys.argv[1]
    emu_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "build", "piezo_emu.wav")
    if len(sys.argv) <= 2:
        subprocess.run([os.path.join(HERE, "build", "chg_headless"), os.path.join(HERE, "roms", "sfxtest_capture.bin"),
                        "14", "NUL", "--wav", emu_path, "--no-piezo"], check=True, capture_output=True)
    rr, rec = load(rec_path)
    re_, emu = load(emu_path)
    br, be = burst_starts(rec, rr), burst_starts(emu, re_)
    lr, le = int(0.65 * rr), int(0.65 * re_)
    p_rec, f = avg_spectrum(rec, rr, br, lr)
    p_noise, _ = avg_spectrum(rec, rr, [b + int(0.8 * rr) for b in br[:-1]], lr)
    p_emu, fe = avg_spectrum(emu, re_, be, le)
    p_emu = np.interp(f, fe, p_emu) * (lr / le)
    sig = np.clip(p_rec - p_noise, 0, None)

    # the response in sixth-octave bands: the band's recorded energy over the
    # band's pin energy (a ratio of sums, so bins where the pin signal is
    # nearly empty do not dominate)
    def third(fc):
        m = (f > fc / 2 ** (1 / 12)) & (f < fc * 2 ** (1 / 12))
        return sig[m].sum() / max(p_emu[m].sum(), 1e-30) if m.any() else np.nan

    grid = np.fft.rfftfreq(NF, 1 / RATE)
    db = np.empty_like(grid)
    for i, fc in enumerate(grid):
        c = min(max(fc, 450.0), 16000.0)
        db[i] = 10 * np.log10(third(c))
    lo, hi = grid < 450, grid > 16000
    db[lo] -= 12 * np.log2(450 / np.maximum(grid[lo], 1))
    db[hi] -= 12 * np.log2(grid[hi] / 16000)
    lgrid = np.log2(np.maximum(grid, 1))

    def smooth(db):
        """a third of an octave moving average: a piezo's response has no
        narrow peaks and notches, whatever one test sound suggests"""
        out = np.empty_like(db)
        for i, lg in enumerate(lgrid):
            m = np.abs(lgrid - lg) <= 1 / 6
            out[i] = db[m].mean()
        return out

    def design(db):
        """minimum phase FIR (real cepstrum, first TAPS samples, windowed)
        for a magnitude response in dB on 'grid', peak at 0 dB"""
        db = np.clip(db - db.max(), -60, 0)
        cep = np.fft.irfft(np.log(np.maximum(10 ** (db / 20), 1e-6)), n=NF)
        fold = np.zeros(NF)
        fold[0] = cep[0]
        fold[1:NF // 2] = 2 * cep[1:NF // 2]
        fold[NF // 2] = cep[NF // 2]
        h = np.fft.irfft(np.exp(np.fft.rfft(fold)), n=NF)[:TAPS]
        return h * np.hanning(2 * TAPS)[TAPS:]

    # The ratio alone credits the piezo's own distortion (its second harmonic
    # of the sweeps, which the 50% square wave does not have) to the response
    # at those frequencies. So the target is corrected until the filtered pin
    # signal spreads its energy over the sixth octaves the way the recording
    # does: a linear filter's nearest to the device's tone.
    centres = 450 * 2 ** (np.arange(0, 32) / 6)
    centres = centres[centres < 16000]
    band = lambda p, fr, c: p[(fr > c / 2 ** (1 / 12)) & (fr < c * 2 ** (1 / 12))].sum()
    rec_b = np.array([band(sig, f, c) for c in centres])
    rec_b /= rec_b.sum()
    emu_on_grid = np.interp(grid, f, p_emu)
    for _ in range(8):
        h = design(db)
        out = emu_on_grid * np.abs(np.fft.rfft(h, n=NF)) ** 2
        out_b = np.array([band(out, grid, c) for c in centres])
        out_b /= out_b.sum()
        corr = 10 * np.log10(np.maximum(rec_b, 1e-12) / np.maximum(out_b, 1e-12))
        db = db + 0.5 * np.interp(np.log(np.maximum(grid, 1)), np.log(centres), np.clip(corr, -20, 20))
        db = smooth(db)
    h = design(db)
    print("band match, recorded vs filtered energy share (sixth octaves):")
    out = emu_on_grid * np.abs(np.fft.rfft(h, n=NF)) ** 2
    out_b = np.array([band(out, grid, c) for c in centres])
    out_b /= out_b.sum()
    for c, a, b in zip(centres, rec_b, out_b):
        if a > 0.005 or b > 0.005:
            print("   %6.0f Hz  recording %5.1f%%  emulator %5.1f%%" % (c, 100 * a, 100 * b))

    # loudness: the filtered capture effect as loud (RMS) as the unfiltered
    x = emu - emu.mean()
    if re_ != RATE:
        x = np.interp(np.arange(0, len(x) * RATE / re_) * re_ / RATE, np.arange(len(x)), x)
    y = np.convolve(x, h)[:len(x)]
    h *= np.sqrt(np.mean(x ** 2) / np.mean(y ** 2))
    # never past full scale at the emulator's default volume (0.5): the worst
    # case is a steady square wave (fundamental 4/pi of its 0.5 swing) right
    # on the resonance
    h *= min(1.0, 1.0 / (np.abs(np.fft.rfft(h, n=NF)).max() * 0.5 * 4 / np.pi * 0.5))

    report = []
    resp = np.abs(np.fft.rfft(h, n=NF))
    for fc in (250, 500, 1000, 2000, 3150, 4000, 5000, 6300, 8000, 10000, 16000):
        report.append("   %5d Hz %+6.1f dB" % (fc, 20 * np.log10(np.interp(fc, grid, resp) / resp.max())))
    out = ["/* Generated by tools/fit_piezo.py from a recording of a real CHGame playing",
           "   CHChess's capture effect: what the piezo (with the room and the",
           "   microphone) makes of the pin signal, as a %d-tap minimum phase FIR at" % TAPS,
           "   48 kHz. Do not edit; fit again from a new recording instead.",
           "",
           "   The filter's response, relative to its peak:"] + report + [" */",
           "#include \"audio.h\"", "",
           "const int chg_piezo_taps = %d;" % TAPS,
           "const float chg_piezo_fir[%d] = {" % TAPS]
    for i in range(0, TAPS, 6):
        out.append("    " + ", ".join("%.9ef" % v for v in h[i:i + 6]) + ",")
    out.append("};")
    open(os.path.join(HERE, "src", "piezo_filter.c"), "w", newline="\n").write("\n".join(out) + "\n")
    print("\n".join(report))
    print("src/piezo_filter.c written")


if __name__ == "__main__":
    main()
