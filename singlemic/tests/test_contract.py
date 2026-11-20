import copy
import importlib.util
import json
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('model_contract', ROOT / 'tools/model_contract.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def test_matching_contract(tmp_path):
    header = tmp_path / 'contract.h'
    module.generate(ROOT / 'models/gtcrn_stream.espdl', ROOT / 'models/gtcrn_stream.json', header)
    assert 'stateCount = 14' in header.read_text()


@pytest.mark.parametrize('kind', ['hash', 'shape', 'cache', 'sample_rate', 'dtype', 'cache_exponent'])
def test_contract_rejects_mismatched_artifacts(tmp_path, kind):
    manifest = json.loads((ROOT / 'models/gtcrn_stream.json').read_text())
    if kind == 'hash': manifest['model_sha256'] = '0' * 64
    if kind == 'sample_rate': manifest['sample_rate'] = 8000
    if kind == 'shape': next(p for p in manifest['inputs'] if p['name'] == 'spec')['shape'][2] = 10
    if kind == 'cache': manifest['outputs'][0]['shape'] = [1]
    if kind == 'dtype': manifest['inputs'][0]['dtype'] = 3
    if kind == 'cache_exponent':
        cache = manifest['states'][0]['output']
        next(p for p in manifest['outputs'] if p['name'] == cache)['exponents'][0] += 1
    path = tmp_path / 'bad.json'
    path.write_text(json.dumps(manifest))
    with pytest.raises(ValueError):
        module.generate(ROOT / 'models/gtcrn_stream.espdl', path, tmp_path / 'contract.h')
