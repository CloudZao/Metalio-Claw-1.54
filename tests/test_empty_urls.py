"""Run URL regression tests with GCC/G++ (including blank open-source config).

Usage: python tests/test_empty_urls.py
Production source is never modified. A timeout catches the empty-host loop.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "main/cloudzao_endpoints.c").read_text(encoding="utf-8")
pattern = r'(const char ([a-z0-9_]+)\[\]\s*=\s*)"[^"]*"'
for scenario in ("current_config", "empty_host", "empty_paths", "open_source"):
    def replace(match):
        name = match[2]
        blank = ((scenario == "empty_host" and name == "cloudzao_host") or
                 (scenario == "empty_paths" and name.startswith("cloudzao_path_")) or
                 (scenario == "open_source" and not name.startswith("xiaozhi_")))
        return match[1] + '""' if blank else match[0]

    with tempfile.TemporaryDirectory(prefix="url-test-") as directory:
        temp = Path(directory)
        fixture = temp / "endpoints.c"
        fixture.write_text(re.sub(pattern, replace, source), encoding="utf-8")
        obj, exe = temp / "endpoints.o", temp / "test.exe"
        subprocess.run(["gcc", "-Wall", "-Wextra", "-Werror", "-I", str(root / "main"),
                        "-c", str(fixture), "-o", str(obj)], check=True)
        subprocess.run(["g++", "-std=c++17", "-DAPI_HTTP_DEBUG=1", "-Wall", "-Wextra", "-Werror",
                        "-I", str(root / "main"), str(root / "tests/url_empty_test.cc"),
                        str(obj), "-o", str(exe)], check=True)
        result = subprocess.run([str(exe)], check=True, timeout=3, capture_output=True)
        assert not result.stdout and not result.stderr, "HTTP helpers leaked log output"
        print(f"PASS: {scenario}", flush=True)
