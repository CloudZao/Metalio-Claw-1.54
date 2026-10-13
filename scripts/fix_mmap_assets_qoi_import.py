"""Apply the optional-QOI import fix to the installed asset generator."""
from pathlib import Path
import sys


def patch_generator(path):
    path = Path(path)
    source = path.read_text(encoding="utf-8")
    broken = """        qoi_module = importlib.import_module('qoi-conv.qoi')
        Qoi = qoi_module.Qoi
        replace_extension = qoi_module.replace_extension

        if convert_to_qoi:
"""
    fixed = """        if convert_to_qoi:
            qoi_module = importlib.import_module('qoi-conv.qoi')
"""
    if broken in source:
        path.write_text(source.replace(broken, fixed), encoding="utf-8")
        print("Fixed optional QOI import in asset generator")
    elif fixed not in source and "importlib.import_module('qoi-conv.qoi')" in source:
        raise RuntimeError("Asset generator changed; review QOI import compatibility")


if __name__ == "__main__":
    patch_generator(sys.argv[1])
