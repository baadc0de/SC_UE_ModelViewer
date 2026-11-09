"""Make an uncompressed GNU tar fixture from an export root; never extracts anything."""
import argparse
from pathlib import Path
import tarfile

parser = argparse.ArgumentParser()
parser.add_argument("folder", type=Path)
parser.add_argument("destination", type=Path)
args = parser.parse_args()
root = args.folder.resolve(strict=True)
files = sorted((p for p in root.rglob("*") if p.is_file()), key=lambda p: (p.name != "scene.json", p.relative_to(root).as_posix()))
args.destination.parent.mkdir(parents=True, exist_ok=True)
with tarfile.open(args.destination, "w", format=tarfile.GNU_FORMAT) as archive:
    for path in files:
        if path.is_symlink() or not path.resolve().is_relative_to(root):
            raise ValueError(f"Unsafe package entry: {path}")
        archive.add(path, arcname=path.relative_to(root).as_posix(), recursive=False)
print(f"TAR files={len(files)} bytes={args.destination.stat().st_size}")
