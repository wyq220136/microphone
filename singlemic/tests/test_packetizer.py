from pathlib import Path
import shutil
import subprocess
import pytest


def test_packet_continuity_raw_fallback_and_session_reset(tmp_path):
    compiler = shutil.which("g++")
    if not compiler:
        pytest.skip("MinGW g++ is required")
    root = Path(__file__).resolve().parents[1]
    executable = tmp_path / "packetizer.exe"
    subprocess.run([compiler, "-std=c++17", "-O1", "-static", "-I", str(root),
                    str(root / "tests/packetizer_host.cpp"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=5)
