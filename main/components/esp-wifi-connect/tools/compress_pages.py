import gzip
import sys
from pathlib import Path
source, destination = map(Path, sys.argv[1:])
destination.mkdir(parents=True, exist_ok=True)
for name in ("wifi_configuration.html", "wifi_configuration_done.html"):
    data = (source / name).read_bytes()
    (destination / (name + ".gz")).write_bytes(gzip.compress(data, compresslevel=9, mtime=0))
