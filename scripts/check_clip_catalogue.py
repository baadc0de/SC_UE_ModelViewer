"""Check a CLI clip catalogue against a decomposed scene animation index."""
import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("catalogue", type=Path)
    parser.add_argument("scene", type=Path)
    args = parser.parse_args()
    catalogue = json.loads(args.catalogue.read_text())
    scene = json.loads(args.scene.read_text())
    ids = {clip["id"] for clip in catalogue["clips"]}
    names = {clip["name"] for clip in scene["root_entity"].get("animations", [])}
    wildcards = {name for name in ids | names if "*" in name or "?" in name}
    if ids != names or wildcards:
        raise SystemExit(
            f"FAIL: catalogue-only={sorted(ids - names)}, "
            f"scene-only={sorted(names - ids)}, wildcards={sorted(wildcards)}"
        )
    print(f"PASS: {len(ids)} catalogue IDs == {len(names)} scene index IDs; no wildcard names")


if __name__ == "__main__":
    main()
