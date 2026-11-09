"""Validate the animation-disabled C/CLI package without loading geometry in Python."""
import json
import sys
import tarfile

with tarfile.open(sys.argv[1]) as archive:
    members = archive.getmembers()
    assert members and members[0].name.endswith("/scene.json")
    assert not any("/animations/" in member.name for member in members)
    scene = json.load(archive.extractfile(members[0]))

def check(value):
    if isinstance(value, dict):
        for key, child in value.items():
            if key == "animations":
                assert child == [], (key, child)
            check(child)
    elif isinstance(value, list):
        for child in value:
            check(child)

check(scene)
print("animations=0 / --no-animations: no animation sidecars or scene clip entries")
