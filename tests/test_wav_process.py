"""IR processing: resampler, trim, fade, cuts, blend, minimum phase, CLI."""

import itertools
import math
import random
import struct
import wave

import pytest

from fb200 import cli
from fb200.errors import WavError
from fb200.pedal import FB200Device
from fb200.wav import (
    IR_LENGTH,
    align_lag,
    blend_irs,
    butterworth2,
    fade_out,
    onset,
    process_ir,
    read_wav,
    resample,
    wav_to_ir,
    write_wav,
)


def write_pcm16(path, samples, channels=1, rate=44100):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", round(s * 32767)) for s in samples))


def decaying_ir(n=3000, delay=0, seed=1):
    rng = random.Random(seed)
    return [0.0] * delay + [rng.uniform(-1, 1) * math.exp(-i / 300) * 0.9 for i in range(n)]


def tone(freq, rate, n, amp=0.5):
    return [amp * math.sin(2 * math.pi * freq * i / rate) for i in range(n)]


def rms(x):
    return math.sqrt(sum(v * v for v in x) / len(x))


def gain_at(x, freq, rate):
    """Magnitude of the DFT bin at ``freq`` (single-bin Goertzel-style sum)."""
    re = sum(v * math.cos(2 * math.pi * freq * i / rate) for i, v in enumerate(x))
    im = sum(v * math.sin(2 * math.pi * freq * i / rate) for i, v in enumerate(x))
    return math.hypot(re, im)


# --- default path: unchanged from the stock-editor behaviour -----------------

def test_plain_44k_mono_default_equals_raw_samples(tmp_path):
    path = tmp_path / "ir.wav"
    src = decaying_ir(2000)
    write_pcm16(path, src)
    raw = read_wav(path)[0][0]
    ir = process_ir(path)
    assert len(ir) == IR_LENGTH
    assert ir == raw[:IR_LENGTH]          # bit-exact: no resample, no fade, no gain
    assert ir == wav_to_ir(path)


def test_plain_short_file_zero_pads(tmp_path):
    path = tmp_path / "short.wav"
    write_pcm16(path, [0.5] * 10)
    ir = process_ir(path)
    assert len(ir) == IR_LENGTH
    assert ir[10:] == [0.0] * (IR_LENGTH - 10)


def test_bytes_source(tmp_path):
    path = tmp_path / "ir.wav"
    write_pcm16(path, decaying_ir(600))
    assert process_ir(path.read_bytes()) == process_ir(path)


# --- resampler ---------------------------------------------------------------

@pytest.mark.parametrize("src_rate", [48000, 96000, 22050, 88200])
def test_resampler_matches_scipy(src_rate):
    signal = pytest.importorskip("scipy.signal")
    from fractions import Fraction

    n = 2000
    # band-limited content well inside both filters' passbands
    x = [sum(0.2 * math.sin(2 * math.pi * f * i / src_rate + f) for f in (220, 1500, 7000))
         for i in range(n)]
    ours = resample(x, src_rate, 44100)
    r = Fraction(44100, src_rate)
    ref = signal.resample_poly(x, r.numerator, r.denominator, window=("kaiser", 10.0))
    assert len(ours) == len(ref)
    lo, hi = len(ours) // 4, 3 * len(ours) // 4   # away from the edge transients
    err = max(abs(a - b) for a, b in zip(ours[lo:hi], ref[lo:hi]))
    assert err < 5e-5                 # measured 3e-6..1.1e-5


def test_resampler_rejects_tone_above_new_nyquist():
    # 30 kHz at 96 kHz would alias to 14.1 kHz at 44.1 kHz
    x = tone(30000, 96000, 4800)
    y = resample(x, 96000, 44100)
    mid = y[600:-600]
    assert 20 * math.log10(rms(mid) / rms(x)) < -60      # measured -112 dB
    # a passband tone survives
    y2 = resample(tone(1000, 96000, 4800), 96000, 44100)[300:300 + 1323]  # 30 periods
    assert rms(y2) == pytest.approx(0.5 / math.sqrt(2), rel=1e-3)


def test_resampler_max_out_is_head_of_full_result():
    x = decaying_ir(3000)
    full = resample(x, 48000, 44100)
    assert resample(x, 48000, 44100, max_out=500) == full[:500]


def test_resampler_rejects_bad_rates():
    with pytest.raises(WavError):
        resample([0.0], 0, 44100)
    with pytest.raises(WavError):
        resample([0.0], 44100, -1)


# --- channel, trim, truncation, fade, normalize ------------------------------

def test_channel_choice(tmp_path):
    path = tmp_path / "st.wav"
    frames = []
    for _ in range(20):
        frames += [0.5, -0.25]
    write_pcm16(path, frames, channels=2)
    assert process_ir(path)[0] == pytest.approx(0.5, abs=1e-4)
    assert process_ir(path, channel="right")[0] == pytest.approx(-0.25, abs=1e-4)
    assert process_ir(path, channel="sum")[0] == pytest.approx(0.125, abs=1e-4)
    with pytest.raises(WavError):
        process_ir(path, channel="middle")


def test_onset_threshold():
    x = [0.0] * 10 + [0.0005, 0.002, 1.0, 0.5]
    assert onset(x) == 11            # 0.0005 < -60 dB (0.001), 0.002 >= it
    assert onset([0.0] * 5) == 0


def test_trim_keeps_preroll(tmp_path):
    path = tmp_path / "late.wav"
    src = decaying_ir(1000, delay=300)
    write_pcm16(path, src)
    ir = process_ir(path, trim=True)
    first = onset(ir)
    assert first == 8                               # default pre-roll
    assert ir[first:first + 100] == pytest.approx(
        read_wav(path)[0][0][300:400], abs=1e-9)
    assert process_ir(path, trim=True, preroll=0)[0] != 0.0


def test_taps_truncates_with_half_hann_fade(tmp_path):
    path = tmp_path / "long.wav"
    write_pcm16(path, [0.25] * 5000)
    ir = process_ir(path, taps=512)
    assert len(ir) == 512
    assert ir[:448] == pytest.approx([0.25] * 448, abs=1e-4)   # untouched body
    fade = ir[448:]
    assert all(a >= b for a, b in itertools.pairwise(fade))         # monotonic
    assert fade[-1] == 0.0
    assert fade[31] == pytest.approx(0.25 * 0.5, abs=1e-3)       # half way = -6 dB


def test_taps_no_fade_when_nothing_is_cut(tmp_path):
    path = tmp_path / "short.wav"
    write_pcm16(path, [0.25] * 100)
    ir = process_ir(path, taps=4096)
    assert len(ir) == 4096
    assert ir[99] == pytest.approx(0.25, abs=1e-4)
    assert ir[100:] == [0.0] * (4096 - 100)


def test_taps_range(tmp_path):
    path = tmp_path / "x.wav"
    write_pcm16(path, [0.1] * 10)
    for bad in (0, 4097):
        with pytest.raises(WavError):
            process_ir(path, taps=bad)


def test_fade_out_shape():
    y = fade_out([1.0] * 8, 4)
    assert y[:4] == [1.0] * 4
    assert y[4:] == pytest.approx([0.5 * (1 + math.cos(math.pi * k / 4)) for k in (1, 2, 3, 4)])


def test_normalize(tmp_path):
    path = tmp_path / "q.wav"
    write_pcm16(path, [0.0, 0.2, -0.4, 0.1])
    ir = process_ir(path, normalize=True)
    assert max(abs(v) for v in ir) == pytest.approx(1.0)
    assert ir[2] == pytest.approx(-1.0)


# --- cuts --------------------------------------------------------------------

@pytest.mark.parametrize("kind,freq", [("high", 80.0), ("low", 5000.0)])
def test_butterworth_minus_3db_at_corner_and_matches_scipy(kind, freq):
    rate = 44100
    n = 44100
    x = tone(freq, rate, n, amp=1.0)
    y = butterworth2(x, kind, freq, rate)
    ratio = rms(y[n // 2:]) / rms(x[n // 2:])
    assert 20 * math.log10(ratio) == pytest.approx(-3.01, abs=0.05)
    signal = pytest.importorskip("scipy.signal")
    b, a = signal.butter(2, freq, btype=kind, fs=rate)
    impulse = [1.0] + [0.0] * 999
    ref = signal.lfilter(b, a, impulse)
    assert butterworth2(impulse, kind, freq, rate) == pytest.approx(list(ref), abs=1e-12)


def test_cut_options_applied(tmp_path):
    path = tmp_path / "imp.wav"
    write_pcm16(path, [1.0] + [0.0] * 3000)
    plain = process_ir(path, taps=4096)
    hp = process_ir(path, taps=4096, lowcut=200.0)
    lp = process_ir(path, taps=4096, highcut=2000.0)
    assert gain_at(plain, 50, 44100) == pytest.approx(1.0, abs=1e-3)
    assert gain_at(hp, 50, 44100) < 0.1        # 2 octaves below: about -24 dB
    assert gain_at(lp, 8000, 44100) < 0.1
    assert gain_at(lp, 100, 44100) == pytest.approx(1.0, abs=0.01)
    with pytest.raises(WavError):
        process_ir(path, lowcut=30000.0)


# --- blend -------------------------------------------------------------------

def test_align_lag_finds_delay():
    a = decaying_ir(800, delay=40, seed=3)
    b = decaying_ir(800, delay=5, seed=3)
    assert align_lag(a, b) == 35


def test_align_lag_refines_beyond_onset():
    # b's onset is a small pre-echo 10 samples before its main (matching) body
    body = decaying_ir(600, seed=4)
    a = [0.0] * 20 + body
    b = [0.0] * 10 + [0.05] + [0.0] * 9 + [0.0] * 10 + body
    assert onset(b, -60) == 10
    assert align_lag(a, b) == -10


def test_blend_mixes_aligned(tmp_path):
    ir = decaying_ir(800, seed=5)
    a_path, b_path = tmp_path / "a.wav", tmp_path / "b.wav"
    write_pcm16(a_path, [0.0] * 10 + ir)
    write_pcm16(b_path, [0.0] * 60 + [0.5 * v for v in ir])
    a = process_ir(a_path)
    out = process_ir(a_path, blend=(b_path, 0.3))
    expect = [0.7 * v + 0.3 * 0.5 * v for v in a]
    assert out == pytest.approx(expect, abs=1e-4)
    with pytest.raises(WavError):
        blend_irs([1.0], [1.0], 1.5)


# --- minimum phase -----------------------------------------------------------

def test_minphase_keeps_magnitude_and_front_loads_energy(tmp_path):
    np = pytest.importorskip("numpy")
    rng = random.Random(7)
    # a mixed-phase IR: delayed, with a strong late reflection
    x = [0.0] * 50 + [rng.uniform(-1, 1) * math.exp(-i / 60) for i in range(300)]
    x[200] += 0.8
    path = tmp_path / "mixed.wav"
    write_pcm16(path, [v / 2 for v in x])
    src = process_ir(path, taps=1024)
    mp = process_ir(path, taps=1024, minphase=True)
    mag_src = np.abs(np.fft.rfft(src, 8192))
    mag_mp = np.abs(np.fft.rfft(mp, 8192))
    assert np.max(np.abs(mag_mp - mag_src)) < 1e-3 * np.max(mag_src)
    e_src = np.cumsum(np.square(src))
    e_mp = np.cumsum(np.square(mp))
    assert e_mp[-1] == pytest.approx(e_src[-1], rel=1e-3)
    assert np.all(e_mp >= e_src - 1e-6 * e_src[-1])     # minimum phase: fastest energy
    assert abs(mp[0]) > 0.01                              # no leading silence


def test_minphase_without_numpy_errors(monkeypatch, tmp_path):
    import builtins

    real_import = builtins.__import__

    def no_numpy(name, *args, **kwargs):
        if name == "numpy":
            raise ImportError("no numpy")
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr(builtins, "__import__", no_numpy)
    path = tmp_path / "x.wav"
    write_pcm16(path, [0.5, 0.1])
    with pytest.raises(WavError, match=r"fb200-tools\[ir\]"):
        process_ir(path, minphase=True)


# --- WAV write and CLI -------------------------------------------------------

def test_write_wav_round_trip(tmp_path):
    x = decaying_ir(700)
    path = tmp_path / "out.wav"
    write_wav(path, x)
    chans, rate = read_wav(path)
    assert rate == 44100 and len(chans) == 1
    assert chans[0] == pytest.approx(x, abs=1e-7)
    wavfile = pytest.importorskip("scipy.io.wavfile")
    r, data = wavfile.read(path)
    assert r == 44100 and data.dtype.name == "float32" and len(data) == 700


def test_cli_ir_process_out_round_trip(tmp_path):
    src = tmp_path / "cab.wav"
    write_pcm16(src, decaying_ir(3000, delay=100), channels=1, rate=48000)
    out = tmp_path / "cab-44k.wav"
    assert cli.main(["ir", "process", str(src), "--out", str(out),
                     "--trim", "--taps", "512", "--lowcut", "40", "--normalize"]) == 0
    chans, rate = read_wav(out)
    assert rate == 44100
    expect = process_ir(src, trim=True, taps=512, lowcut=40.0, normalize=True)
    assert chans[0] == pytest.approx(expect, abs=1e-7)
    assert len(chans[0]) == 512


def test_cli_ir_process_default_is_stock_shape(tmp_path):
    src = tmp_path / "cab.wav"
    write_pcm16(src, decaying_ir(2000))
    out = tmp_path / "o.wav"
    assert cli.main(["ir", "process", str(src), "-o", str(out)]) == 0
    assert read_wav(out)[0][0] == pytest.approx(wav_to_ir(src), abs=1e-7)


class FakeDevice:
    import_wav = FB200Device.import_wav      # the real conversion, fake upload

    def __init__(self):
        self.calls = []
        self.transport = type("T", (), {"close": lambda self: None})()

    def ir_import(self, slot, name, samples, progress=None):
        self.calls.append((slot, name, list(samples)))
        return True


def test_cli_ir_import_default_uploads_stock_samples(monkeypatch, tmp_path):
    src = tmp_path / "Bass Cab.wav"
    write_pcm16(src, decaying_ir(2000))
    device = FakeDevice()
    monkeypatch.setattr(cli, "_open_device", lambda: device)
    assert cli.main(["ir", "import", "3", str(src)]) == 0
    slot, name, samples = device.calls[0]
    assert (slot, name) == (3, "Bass Cab")
    assert samples == wav_to_ir(src)


def test_cli_ir_import_options_and_blend_arg(monkeypatch, tmp_path):
    a, b = tmp_path / "a.wav", tmp_path / "b.wav"
    write_pcm16(a, decaying_ir(900, delay=30, seed=1))
    write_pcm16(b, decaying_ir(900, delay=70, seed=2))
    device = FakeDevice()
    monkeypatch.setattr(cli, "_open_device", lambda: device)
    assert cli.main(["ir", "import", "1", str(a), "--taps", "512", "--channel", "sum",
                     "--blend", f"{b}:0.25"]) == 0
    assert device.calls[0][2] == process_ir(a, taps=512, channel="sum", blend=(str(b), 0.25))
    assert cli.main(["ir", "import", "1", str(a), "--taps", "2048"]) == 1
    with pytest.raises(SystemExit):
        cli.main(["ir", "import", "1", str(a), "--blend", "b.wav"])
    with pytest.raises(SystemExit):
        cli.main(["ir", "import", "1", str(a), "--blend", "b.wav:2"])
