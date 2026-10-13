"""Test PNG/JPEG splitting when optional QOI tooling is unavailable."""
from pathlib import Path
import importlib.util
import runpy
import sys
import tempfile
from unittest.mock import patch

from PIL import Image

root = Path(__file__).resolve().parents[1]
fix = runpy.run_path(str(root / "scripts/fix_mmap_assets_qoi_import.py"))["patch_generator"]
generator = root / "managed_components/espressif__esp_mmap_assets/py_tool/spiffs_assets_gen.py"
real_import = __import__("importlib").import_module


def no_qoi(name, *args, **kwargs):
    if name == "qoi-conv.qoi":
        raise ModuleNotFoundError("No module named 'qoi-conv'")
    return real_import(name, *args, **kwargs)


with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    copy = temp / "generator.py"
    copy.write_bytes(generator.read_bytes())
    fix(copy)
    patched = copy.read_bytes()
    fix(copy)
    assert copy.read_bytes() == patched, "Patch must be idempotent"
    spec = importlib.util.spec_from_file_location("asset_generator_test", copy)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    with patch("importlib.import_module", side_effect=no_qoi):
        for extension in (".png", ".jpg"):
            output = temp / extension[1:]
            output.mkdir()
            image = Image.new("RGB", (24, 35), "red")
            assert module.split_image(image, 16, str(output), extension, False) == (24, 35, 3)
            assert len(list(output.glob("*" + extension))) == 3
            print(f"PASS: {extension} splitting without QOI")
        try:
            module.split_image(image, 16, str(output), ".png", True)
        except ModuleNotFoundError:
            print("PASS: QOI dependency remains required when enabled")
        else:
            raise AssertionError("QOI conversion must require QOI tooling")
