"""Generate optional board self-test vectors without NumPy/PyTorch on build host."""
import json
from pathlib import Path
import sys


def generate(golden_path, manifest_path, header_path):
    golden = json.loads(golden_path.read_text())
    manifest = json.loads(manifest_path.read_text())
    if golden['model_sha256'] != manifest['model_sha256']:
        raise ValueError('Golden vectors do not match production model')
    frames = golden['frames']
    if len(frames) < 4 or any(len(f[key]) != 514 for f in frames for key in ('spec', 'enh')):
        raise ValueError('Incomplete continuous test vectors')
    text = '#pragma once\n#include <cstdint>\nnamespace GoldenStream {\n'
    text += f'inline constexpr int count = {len(frames)};\n'
    text += f'inline constexpr int inputExponent = {golden["input_exponent"]};\n'
    text += f'inline constexpr int outputExponent = {golden["output_exponent"]};\n'
    for key in ('spec', 'enh'):
        text += f'inline constexpr int16_t {key}[count][514] = {{\n'
        text += ',\n'.join('{' + ','.join(map(str, frame[key])) + '}' for frame in frames)
        text += '\n};\n'
    text += '}\n'
    header_path.parent.mkdir(parents=True, exist_ok=True)
    if not header_path.exists() or header_path.read_text() != text:
        header_path.write_text(text)


if __name__ == '__main__':
    generate(*(Path(p) for p in sys.argv[1:]))
