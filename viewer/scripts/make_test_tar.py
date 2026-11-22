"""Make a contract USTAR/path-only-PAX fixture (or legacy GNU); never extract."""
import argparse
from pathlib import Path
import tarfile

parser = argparse.ArgumentParser()
parser.add_argument("folder", type=Path)
parser.add_argument("destination", type=Path)
parser.add_argument("--gnu", action="store_true", help="Exercise legacy GNU long-name records")
args = parser.parse_args()
root = args.folder.resolve(strict=True)
files = sorted((p for p in root.rglob("*") if p.is_file()), key=lambda p: (p.name != "scene.json", p.relative_to(root).as_posix()))
args.destination.parent.mkdir(parents=True, exist_ok=True)
with tarfile.open(args.destination, "w", format=tarfile.USTAR_FORMAT) as archive:
    for path in files:
        if path.is_symlink() or not path.resolve().is_relative_to(root):
            raise ValueError(f"Unsafe package entry: {path}")
        info = tarfile.TarInfo(path.relative_to(root).as_posix())
        info.size = path.stat().st_size
        archive.format = tarfile.GNU_FORMAT if args.gnu else tarfile.USTAR_FORMAT
        if not args.gnu:
            try:
                info.tobuf(format=tarfile.USTAR_FORMAT, encoding="utf-8", errors="strict")
            except (ValueError, UnicodeError):
                # USTAR attempts name/prefix splitting first; only path needs PAX.
                archive.format = tarfile.PAX_FORMAT
        with path.open("rb") as content:
            archive.addfile(info, content)
print(f"TAR files={len(files)} bytes={args.destination.stat().st_size} format={'GNU' if args.gnu else 'USTAR/path-only-PAX'}")
