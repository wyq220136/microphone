"""Validate the model/manifest pair and generate a dependency-free C++ contract."""
import hashlib
import json
import math
from pathlib import Path
import sys


def require(condition, message):
    if not condition:
        raise ValueError(message)


def generate(model_path, manifest_path, header_path):
    data = model_path.read_bytes()
    m = json.loads(manifest_path.read_text())
    require(m['schema_version'] == 1 and m['target'] == 'c' and m['quant_type'] == 'w16a16', 'Unsupported model contract')
    require(len(data) == m['model_size'] <= 0x200000, 'Model size mismatch or partition too small')
    require(hashlib.sha256(data).hexdigest() == m['model_sha256'], 'Model SHA256 mismatch')
    require(m['sample_rate'] == 16000 and m['fft_size'] == 512 and m['hop_size'] == 256, 'Invalid STFT contract')
    require(m['window'] == 'sqrt_periodic_hann', 'Invalid window')
    ins = {p['name']: p for p in m['inputs']}
    outs = {p['name']: p for p in m['outputs']}
    require(ins['spec']['shape'] == [1, 257, 1, 2], 'Invalid complex input shape')
    require(outs['enh']['shape'] == [1, 257, 1, 2], 'Invalid complex output shape')
    require(len(ins) == len(outs) == len(m['states']) + 1 and len(m['states']) == 14, 'Invalid state count')
    for state in m['states']:
        require(ins[state['input']]['shape'] == outs[state['output']]['shape'], f'Incompatible cache: {state}')
        require(ins[state['input']]['dtype'] == outs[state['output']]['dtype'] and
                ins[state['input']]['exponents'] == outs[state['output']]['exponents'],
                f'Cache input/output quantization must match: {state}')
    lines = ['#pragma once', '#include <cstddef>', '#include <cstdint>',
             'namespace ModelContract {',
             'struct Port { const char *name; int dtype, exponent, rank; int dims[4]; size_t elements; };',
             f'inline constexpr size_t bytes = {len(data)};',
             f'inline constexpr char sha256[] = "{m["model_sha256"]}";',
             f'inline constexpr size_t stateCount = {len(m["states"])};']
    def port(p):
        require(0 < len(p['shape']) <= 4 and all(d > 0 for d in p['shape']), 'Invalid tensor shape')
        require(len(p['exponents']) <= 1 and p['dtype'] in (1, 5), 'Unsupported tensor quantization')
        return '{"%s", %d, %d, %d, {%s}, %d}' % (
            p['name'], p['dtype'], (p['exponents'] or [0])[0], len(p['shape']),
            ','.join(map(str, p['shape'] + [0] * (4-len(p['shape'])))), math.prod(p['shape']))
    lines += [f'inline constexpr Port spec = {port(ins["spec"])};',
              f'inline constexpr Port enhanced = {port(outs["enh"])};',
              'inline constexpr Port inputs[] = {' + ','.join(port(ins[s['input']]) for s in m['states']) + '};',
              'inline constexpr Port outputs[] = {' + ','.join(port(outs[s['output']]) for s in m['states']) + '};']
    norm, level = m['normalization'], m['output_level_match']
    for name, value in {'normalize': norm.get('enabled', False), 'match': level.get('enabled', False)}.items():
        lines.append(f'inline constexpr bool {name} = {str(bool(value)).lower()};')
    for name, value in {'targetRms': norm.get('target_rms', 0.1),
                        'minInputGain': norm.get('min_gain', 0.1), 'maxInputGain': norm.get('max_gain', 10),
                        'minOutputGain': 10**(level.get('min_gain_db', 0)/20),
                        'maxOutputGain': 10**(level.get('max_gain_db', 24)/20)}.items():
        lines.append(f'inline constexpr float {name} = {float(value):.9e}f;')
    lines.append('}')
    header_path.parent.mkdir(parents=True, exist_ok=True)
    text = '\n'.join(lines) + '\n'
    if not header_path.exists() or header_path.read_text() != text:
        header_path.write_text(text)
    print('Model contract verified:', m['model_sha256'])


if __name__ == '__main__':
    generate(*(Path(p) for p in sys.argv[1:]))
