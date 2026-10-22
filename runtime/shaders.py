"""Stable shader IDs shared with the launcher through the packaged TSV catalog."""
from pathlib import Path
if __package__:
    from .engines import CORES
else:
    from engines import CORES


def catalog():
    source = Path(__file__).resolve().parents[1] / "assets/shaders/catalog.tsv"
    entries = []
    for line in source.read_text().splitlines():
        if not line or line.startswith("#"): continue
        identity, name, stem, asset_id, description = line.split("\t")
        if int(identity) != len(entries): raise ValueError("Invalid shader catalog order")
        entries.append(dict(name=name, file=stem, asset_id=int(asset_id), description=description))
    if not entries or entries[0]["file"] != "off" or len({entry["file"] for entry in entries}) != len(entries):
        raise ValueError("Invalid shader catalog")
    return tuple(entries)


SHADERS = catalog()
FILTERS = tuple(entry["file"] for entry in SHADERS)


def supported(core, choice):
    return core in CORES and 0 <= choice < len(SHADERS)
