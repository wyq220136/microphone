from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="session")
def executable(tmp_path_factory):
    compiler = shutil.which("g++")
    if not compiler:
        pytest.skip("MinGW g++ is required")
    path = tmp_path_factory.mktemp("pipeline") / "pipeline.exe"
    subprocess.run([compiler, "-std=c++17", "-O1", "-pthread", "-static",
                    "-I", str(ROOT / "tests/fakes"), "-I", str(ROOT),
                    str(ROOT / "audio_pipeline.cpp"), str(ROOT / "audio_processor.cpp"),
                    str(ROOT / "tests/pipeline_host.cpp"), "-o", str(path)], check=True)
    return path


@pytest.mark.parametrize("scenario", ["normal", "missing", "preflight", "slow", "abort", "pool", "immediate_stop", "stale_failure"])
def test_actual_pipeline_tasks(executable, scenario):
    result = subprocess.run([str(executable), scenario], capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stdout + result.stderr
    assert f"PASS {scenario}" in result.stdout
