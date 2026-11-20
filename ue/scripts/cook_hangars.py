"""WP-C1: cook exported hangars into levels (one per hangar) with pooled meshes/textures.

Run after scripts/export_hangars.sh, with a renderer (runtime textures and Nanite builds):
  UnrealEditor-Cmd SCViewer.uproject -run=pythonscript -script=<abs>/cook_hangars.py -vulkan -AllowCommandletRendering \
      -unattended -nop4 -ddc=SCViewerDDC -SCCookRoot=<abs>/out/hangars [-SCCookOnly=hightech_a_hangar_lrgtop_001]
Levels: /Game/SC/Hangars/Maps/<style>_<hangar>; materials: /Game/SC/Hangars/<style>_<hangar>/Materials;
meshes and textures: /Game/SC/Hangars/Shared. All game-derived (gitignored). Cooked hangars are skipped: to re-cook
one, delete Content/SC/Hangars/Maps/<name>.umap and Content/SC/Hangars/<name>/ first (the shared pool stays).
Each cooked level is stamped with USCCookLibrary::FormatVersion (<name>.umap's .cookformat); a level with an older or
no stamp is reported STALE with that recook instruction, and package_showcase.ps1 refuses it.
Each cook records the hangar's ship capacity (elevator footprint, clear height) in /Game/SC/Hangars/HangarIndex;
-SCCookMeasure records it for hangars cooked before (import and present only, nothing else is saved).
"""
import json
import os
import re
import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cook_audio  # noqa: E402  (WP-A1: the package's audio beside the cooked level)

args = unreal.SystemLibrary.get_command_line()
root = re.search(r'-SCCookRoot=(\S+)', args).group(1)
only = re.search(r'-SCCookOnly=(\S+)', args)
only = set(only.group(1).split(',')) if only else None
measure = '-SCCookMeasure' in args
FORMAT = unreal.SCCookLibrary.cook_format_version()
CONTENT = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_content_dir())


def stamp_path(level):
    """<level>.cookformat beside the level's .umap (Content/SC/... is game-derived, gitignored)."""
    return os.path.join(CONTENT, level[len('/Game/'):] + '.cookformat')


def stamped(level):
    try:
        with open(stamp_path(level), encoding='utf-8') as f:
            return int(f.read().strip())
    except (OSError, ValueError):
        return 0


def stale_bindings(scene_path):
    """UI bindings an export wrote before the screen-role fields (source_item_type, source_owner_resolved,
    source_kiosk_interaction; cache format 30): their panels would cook dark, so the export must be redone first."""
    with open(scene_path, encoding='utf-8') as f:
        scene = json.load(f)
    entries = list(scene.get('children', [])) + [p for i in scene.get('interiors', []) for p in i.get('placements', [])]
    return sum(1 for e in entries for b in e.get('ui_bindings', []) or []
               if not all(k in b for k in ('source_item_type', 'source_owner_resolved', 'source_kiosk_interaction')))


levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
results = []
packages = sorted(os.listdir(os.path.join(root, 'Packages')))
for package in packages:
    # Package folders read like "commonhangarhightech ahangar lrgtop 001socpak_LOD0_TEX0".
    m = re.match(r'(?:common)?hangar(\w+) ahangar (\w+) (\w+?)(?: (\w+))?socpak_', package)
    if not m:
        unreal.log_warning('COOK_SKIP %s (not a hangar package)' % package)
        continue
    style = m.group(1) + '_a'
    base = 'hangar_%s_%s' % (m.group(2), m.group(3))
    name = '%s_%s' % (style, base)
    if only and name not in only:
        continue
    level = '/Game/SC/Hangars/Maps/' + name
    if unreal.EditorAssetLibrary.does_asset_exist(level):
        # The asset registry scanned it at startup: delete Content/SC/Hangars/Maps/<name>.umap and
        # Content/SC/Hangars/<name>/ before launching to re-cook (the shared pool stays).
        if measure:
            env = unreal.SCImportLibrary.import_environment(editor.get_editor_world(), root, 'Packages/%s/scene.json' % package)
            ok = bool(env) and unreal.SCImportLibrary.present_environment(env, 'open', -5.0) is not None and \
                unreal.SCCookLibrary.record_hangar_capacity(env, style, base)
            if env:
                env.destroy_actor()
            results.append('%s measured=%s' % (name, ok))
        elif stamped(level) == FORMAT:
            results.append('%s already cooked (skipped)' % name)
        else:
            results.append('%s STALE (cook format %d, current %d): delete Content/SC/Hangars/Maps/%s.umap and '
                           'Content/SC/Hangars/%s/ and re-run' % (name, stamped(level), FORMAT, name, name))
        unreal.log('COOK_HANGAR ' + results[-1])
        continue
    if measure:
        continue
    stale = stale_bindings(os.path.join(root, 'Packages/%s/scene.json' % package))
    if stale:
        results.append('%s STALE EXPORT (%d ui_bindings without the screen-role fields): re-export it with the current '
                       'exporter first' % (name, stale))
        unreal.log_warning('COOK_HANGAR ' + results[-1])
        continue
    if os.path.exists(stamp_path(level)):
        os.remove(stamp_path(level))  # a stamp is published only after this cook saves
    assert levels.new_level(level, False), level
    world = editor.get_editor_world()
    env = unreal.SCImportLibrary.import_environment(world, root, 'Packages/%s/scene.json' % package)
    if not env:
        results.append('%s import failed' % name)
        continue
    env.set_actor_label('Environment ' + name)
    offset = unreal.SCImportLibrary.present_environment(env, 'open', -5.0)
    unreal.SCCookLibrary.record_hangar_capacity(env, style, base)
    try:
        audio = cook_audio.stage(root, 'Packages/%s/scene.json' % package, name)
    except cook_audio.CookAudioError as e:
        results.append('%s STALE EXPORT (%s)' % (name, e))
        unreal.log_warning('COOK_HANGAR ' + results[-1])
        continue
    env.set_editor_property('cooked_audio', audio[0])
    env.set_editor_property('cooked_audio_scene', audio[1])
    ok = unreal.SCCookLibrary.cook_environment(env, '/Game/SC/Hangars/' + name)
    saved = levels.save_current_level()
    if ok and saved:
        with open(stamp_path(level), 'w', encoding='utf-8') as f:
            f.write('%d\n' % FORMAT)
    results.append('%s offset=(%.1f, %.1f, %.1f) cook=%s level_saved=%s format=%d' % (name, offset.x, offset.y, offset.z, ok, saved, FORMAT))
    unreal.log('COOK_HANGAR ' + results[-1])
unreal.log('COOK_DONE ' + ' | '.join(results))
