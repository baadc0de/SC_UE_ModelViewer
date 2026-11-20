"""WP-A1: carry a package's audio into a cooked level (cook_ships.py, cook_hangars.py).

A cooked level has no package at run time, so its audio travels beside it as loose files the viewer reads the same way
it reads a package: Content/SCAudio/<level>/Packages/<package>/audio.json, the media (Audio/<id>.ogg) and impulse
responses (Audio/ir/<id>.f32) it lists, and Packages/<package>/audio_interiors.json — per interior the scene's
audio_spots, audio_areas, room_volumes and room_connectors (the cooked actor keeps its interior anchors). The packaging settings stage
Content/SCAudio as non-UFS files (DefaultGame.ini); ASCVehicle::BeginPlay loads them (ASCVehicle::CookedAudio). The files
are game-derived: Content/SCAudio is gitignored.
"""
import json
import os
import shutil

import unreal


class CookAudioError(Exception):
    """The level cannot carry its audio (an export without audio.json, or a payload it lists is missing): the cook of
    that level stops and writes no stamp."""


CONTENT = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_content_dir())


def stage(root, scene, level_name):
    """Copy the audio of export root `root`'s `scene` (Packages/<name>/scene.json) to Content/SCAudio/<level_name>.
    Returns (folder relative to the content dir, scene path inside it, files copied). Raises CookAudioError when the export
    has no audio.json (an export before cache format 36: re-export) or a media / impulse-response file it lists is missing;
    the previous staging of the level is replaced only after every file is found."""
    package_dir = os.path.dirname(scene)
    audio_json = os.path.join(root, package_dir, 'audio.json')
    if not os.path.exists(audio_json):
        raise CookAudioError('%s: no audio.json (an export before cache format 36): re-export it' % scene)
    with open(audio_json, encoding='utf-8') as f:
        manifest = json.load(f)
    files = [m['file'] for m in manifest.get('media', {}).values() if m.get('file')]
    files += [e['impulse_response']['file'] for e in manifest.get('environments', {}).values() if e.get('impulse_response')]
    missing = [rel for rel in sorted(set(files)) if not os.path.exists(os.path.join(root, rel))]
    if missing:
        raise CookAudioError('%s: %d audio payloads missing (first: %s): re-export it' % (scene, len(missing), missing[0]))
    folder = 'SCAudio/' + level_name
    target = os.path.join(CONTENT, folder)
    if os.path.isdir(target):
        shutil.rmtree(target)
    os.makedirs(os.path.join(target, package_dir), exist_ok=True)
    shutil.copyfile(audio_json, os.path.join(target, package_dir, 'audio.json'))
    copied = 0
    for rel in sorted(set(files)):
        src = os.path.join(root, rel)
        dst = os.path.join(target, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)
        copied += 1
    with open(os.path.join(root, scene), encoding='utf-8') as f:
        interiors = json.load(f).get('interiors', [])
    sidecar = [{'name': i.get('name', ''), 'audio_spots': i.get('audio_spots', []), 'audio_areas': i.get('audio_areas', []),
                'room_volumes': i.get('room_volumes', []), 'room_connectors': i.get('room_connectors', [])} for i in interiors]
    with open(os.path.join(target, package_dir, 'audio_interiors.json'), 'w', encoding='utf-8') as f:
        json.dump({'version': 1, 'interiors': sidecar}, f)
    unreal.log('COOK_AUDIO %s -> %s files=%d interiors=%d' % (scene, folder, copied, len(sidecar)))
    return folder, scene.replace('\\', '/'), copied
