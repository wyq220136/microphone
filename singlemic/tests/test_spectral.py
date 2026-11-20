"""Exercise the production C++ DSP using an independent host FFT/backend."""
import ctypes
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys

import numpy as np
import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="session")
def run(tmp_path_factory):
    compiler = shutil.which("g++")
    if not compiler:
        pytest.skip("MinGW g++ required for the Windows host DSP test")
    dll = tmp_path_factory.mktemp("spectral") / "spectral.dll"
    subprocess.run([compiler, "-std=c++17", "-O2", "-shared", "-static-libgcc", "-static-libstdc++",
                    "-I", str(ROOT), str(ROOT / "spectral_dsp.cpp"),
                    str(ROOT / "tests/spectral_host.cpp"), "-o", str(dll)], check=True)
    library = ctypes.CDLL(str(dll))
    fn = library.run_dsp
    fn.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                   ctypes.c_float, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_float]
    fn.restype = ctypes.c_int
    def execute(x, gain=1, normalize=False, match=False, reset=False, bias=0):
        x = np.ascontiguousarray(x, dtype=np.int32)
        y = np.zeros_like(x)
        written = fn(x.ctypes.data, len(x), y.ctypes.data, gain, normalize, match, reset, bias)
        assert written == len(x)
        return y
    return execute


@pytest.mark.parametrize("length", [0, 1, 17, 255, 256, 257, 511, 512, 1003, 4096])
def test_identity_and_exact_length(run, length):
    rng = np.random.default_rng(42)
    x = rng.integers(-2000000, 2000001, length, dtype=np.int32)
    y = run(x)
    assert np.max(np.abs(y.astype(np.int64)-x), initial=0) <= 32


@pytest.mark.parametrize("kind", ["silence", "impulse", "sine", "positive", "negative"])
def test_no_pcm16_truncation_or_wrap(run, kind):
    x = np.zeros(1503, np.int32)
    if kind == "impulse": x[503] = 8388607
    if kind == "sine": x[:] = np.rint(7000000*np.sin(np.arange(len(x))*0.13))
    if kind == "positive": x[:] = 8388607
    if kind == "negative": x[:] = -8388608
    y = run(x)
    assert np.max(np.abs(y.astype(np.int64)-x), initial=0) <= 64
    assert np.max(y, initial=0) <= 8388607 and np.min(y, initial=0) >= -8388608


def test_causal_normalization_is_undone(run):
    x = np.rint(400000*np.sin(np.arange(8191)*0.1)).astype(np.int32)
    x[len(x)//2:] //= 8
    assert np.max(np.abs(run(x, normalize=True).astype(np.int64)-x)) <= 32


def test_gain_protection_and_state_reset(run):
    x = np.rint(8000000*np.sin(np.arange(3301)*0.13)).astype(np.int32)
    a = run(x, gain=3)
    assert np.max(a) <= 8388607 and np.min(a) >= -8388608
    assert np.max(np.abs(a.astype(np.int64))) > 32767
    np.testing.assert_array_equal(a, run(x, gain=3, reset=True))


@pytest.mark.parametrize("gain,compensation", [(0.1, 10), (0.01, 10**(24/20)), (2, 1)])
def test_loudness_compensation_and_checkpoint_gain_bounds(run, gain, compensation):
    x = np.rint(400000*np.sin(np.arange(8192)*0.1)).astype(np.int32)
    y = run(x, gain=gain, match=True)
    expected = x.astype(np.float64)*gain*compensation
    assert np.max(np.abs(y-expected)) <= 64


def test_silence_does_not_boost_model_residual_after_speech(run):
    x = np.zeros(4096, np.int32)
    x[:2048] = np.rint(400000*np.sin(np.arange(2048)*0.1))
    matched = run(x, gain=0.1, match=True, bias=0.01)
    plain = run(x, gain=0.1, match=False, bias=0.01)
    # Allow the prior non-silent overlap to drain before checking silent frames.
    np.testing.assert_array_equal(matched[2560:], plain[2560:])


def test_pcm24_receiver_preserves_high_bits():
    path = ROOT.parent / "py_rec/spp_audio_receiver.py"
    spec = importlib.util.spec_from_file_location("spp_receiver", path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    samples = [-8388608, -1000001, -1, 0, 1, 1000001, 8388607]
    wire = b''.join((s & 0xffffff).to_bytes(3, 'little') for s in samples)
    decoded = np.frombuffer(module.pcm24_to_pcm32(wire), dtype='<i4')
    np.testing.assert_array_equal(decoded, np.array(samples, np.int64) * 256)
    with pytest.raises(ValueError):
        module.pcm24_to_pcm32(b'\x00')
