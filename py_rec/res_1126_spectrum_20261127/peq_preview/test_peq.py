import unittest
import numpy as np
from scipy import signal
from simulate_peq import peq_sos, baseline_sos, HERE
import json


class PeqTests(unittest.TestCase):
    def test_center_gain_and_half_gain_bandwidth(self):
        for fs in [16000, 48000]:
            for f0, bw, gain in [(300, 300, -4), (250, 200, -6), (700, 1100, -3), (1000, 90, 6)]:
                sos, p = peq_sos(fs, f0, gain, bandwidth_hz=bw)
                _, h = signal.sosfreqz(sos, worN=[0, p['half_gain_low_hz'], f0, p['half_gain_high_hz'], fs/2], fs=fs)
                np.testing.assert_allclose(20*np.log10(abs(h)), [0, gain/2, gain, gain/2, 0], atol=1e-8)
                self.assertAlmostEqual(p['bandwidth_hz'], bw, places=8)

    def test_q_and_hz_bandwidth_are_equivalent(self):
        a, p = peq_sos(16000, 300, -4, bandwidth_hz=300)
        b, _ = peq_sos(16000, 300, -4, q=p['q'])
        np.testing.assert_allclose(a, b, atol=1e-15)

    def test_no_gain_is_bypass(self):
        rng = np.random.default_rng(1)
        x = rng.normal(size=32000)
        sos, _ = peq_sos(16000, 300, 0, bandwidth_hz=300)
        np.testing.assert_allclose(signal.sosfilt(sos, x), x, atol=1e-12)

    def test_cut_boost_inverse(self):
        a, _ = peq_sos(16000, 300, -6, bandwidth_hz=300)
        b, _ = peq_sos(16000, 300, 6, bandwidth_hz=300)
        _, h = signal.sosfreqz(np.vstack([a, b]), worN=4096)
        np.testing.assert_allclose(abs(h), 1, atol=1e-11)

    def test_causal_streaming_and_stability(self):
        rng = np.random.default_rng(2)
        x = rng.normal(size=32001)
        sos, _ = peq_sos(16000, 300, -4, bandwidth_hz=300)
        full = signal.sosfilt(sos, x)
        state = np.zeros((1, 2))
        parts = []
        for i in range(0, len(x), 76):
            y, state = signal.sosfilt(sos, x[i:i+76], zi=state)
            parts.append(y)
        np.testing.assert_array_equal(full, np.concatenate(parts))
        self.assertLess(max(abs(np.roots(sos[0, 3:]))), 1)

    def test_invalid_parameters(self):
        for kwargs in [{'center_hz': 0, 'bandwidth_hz': 300}, {'center_hz': 8000, 'q': 1},
                       {'center_hz': 300, 'q': 0}, {'center_hz': 300, 'bandwidth_hz': -1},
                       {'center_hz': 300, 'bandwidth_hz': 8000},
                       {'center_hz': 300, 'q': 1, 'bandwidth_hz': 300}]:
            with self.assertRaises(ValueError):
                peq_sos(16000, gain_db=-4, **kwargs)

    def test_baseline_matches_previous_preview_response(self):
        config = json.loads((HERE / 'config.json').read_text())
        actual = baseline_sos(16000, config['baseline'])
        prior = json.loads((HERE.parent / 'higher_order_low_cut_high_boost/filter_parameters.json').read_text())
        expected = np.array(prior['ideal_raw_to_output_sos']['strong_both'])
        freq = np.geomspace(20, 7990, 5000)
        _, a = signal.sosfreqz(actual, worN=freq, fs=16000)
        _, b = signal.sosfreqz(expected, worN=freq, fs=16000)
        np.testing.assert_allclose(a, b, rtol=1e-10, atol=1e-10)


if __name__ == '__main__':
    unittest.main()
