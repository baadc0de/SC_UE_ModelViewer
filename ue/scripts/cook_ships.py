"""Cook exported ships into levels (one per ship) with pooled meshes and textures. The viewer's LoadEntity streams
/Game/SC/Ships/Maps/<entity> instead of exporting from the archive, so a cooked ship also loads where there is no
archive (the Win64 showcase, ue/scripts/package_showcase.ps1).

Export first (decomposed GLB + DDS into a shared root, at full resolution like the hangar cook):
  scripts/starbreaker.sh entity export DRAK_Golem out/showcase/bake --kind decomposed --format glb --dds --lod 0 --mip 0 \
      --skip-existing-assets
then cook with a renderer (runtime textures and Nanite builds):
  UnrealEditor-Cmd SCViewer.uproject -run=pythonscript -script=<abs>/cook_ships.py -AllowCommandletRendering -unattended \
      -nop4 -SCCookRoot=<abs export root> [-SCCookOnly=DRAK_Golem,...]
Levels: /Game/SC/Ships/Maps/<entity>; materials: /Game/SC/Ships/<entity>/Materials; meshes and textures:
/Game/SC/Ships/Shared. All game-derived (gitignored). The ship is cooked as the viewer shows it: landing gear down,
resting on the viewer floor; rigs are baked static (CookEnvironment). Cooked ships are skipped: delete
Content/SC/Ships/Maps/<entity>.umap and Content/SC/Ships/<entity>/ to re-cook (the shared pool stays). Each cooked level
is stamped with USCCookLibrary::FormatVersion (<entity>.umap's .cookformat); a level with an older or no stamp is
reported STALE with that recook instruction, and package_showcase.ps1 refuses it.
"""
import json
import os
import re

import sys

import unreal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cook_audio  # noqa: E402  (WP-A1: the package's audio beside the cooked level)

VIEWER_FLOOR_Z = -5.0  # USCViewerSubsystem's floor: the cooked level streams in at the origin
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
    source_kiosk_interaction; cache format 30): their panels would cook dark; or before interaction_distance (cache format
    40): door panels would be operated from 3 m only. The export must be redone first."""
    with open(scene_path, encoding='utf-8') as f:
        scene = json.load(f)
    entries = list(scene.get('children', [])) + [p for i in scene.get('interiors', []) for p in i.get('placements', [])]
    return sum(1 for e in entries for b in e.get('ui_bindings', []) or []
               if not all(k in b for k in ('source_item_type', 'source_owner_resolved', 'source_kiosk_interaction',
                                           'interaction_distance')))


args = unreal.SystemLibrary.get_command_line()
root = re.search(r'-SCCookRoot=(\S+)', args).group(1)
only = re.search(r'-SCCookOnly=(\S+)', args)
only = set(n.lower() for n in only.group(1).split(',')) if only else None
levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
results = []
for package in sorted(os.listdir(os.path.join(root, 'Packages'))):
    scene = 'Packages/%s/scene.json' % package
    with open(os.path.join(root, scene), encoding='utf-8') as f:
        entity = json.load(f).get('root_entity', {}).get('entity_name', '')
    # Entity exports name their root "EntityClassDefinition.<entity>"; object containers (hangars) are not ships.
    if not entity.startswith('EntityClassDefinition.'):
        continue
    name = entity.split('.', 1)[1]
    if only and name.lower() not in only:
        continue
    level = '/Game/SC/Ships/Maps/' + re.sub(r'[^A-Za-z0-9_]', '_', name)
    if unreal.EditorAssetLibrary.does_asset_exist(level):
        if stamped(level) == FORMAT:
            results.append('%s already cooked (skipped)' % name)
        else:
            results.append('%s STALE (cook format %d, current %d): delete Content/SC/Ships/Maps/%s.umap and Content/SC/Ships/%s/ '
                           'and re-run' % (name, stamped(level), FORMAT, level.rsplit('/', 1)[1], name))
            unreal.log_warning('COOK_SHIP ' + results[-1])
            continue
        unreal.log('COOK_SHIP ' + results[-1])
        continue
    stale = stale_bindings(os.path.join(root, scene))
    if stale:
        results.append('%s STALE EXPORT (%d ui_bindings without the screen-role fields): re-export it with the current '
                       'exporter first' % (name, stale))
        unreal.log_warning('COOK_SHIP ' + results[-1])
        continue
    if os.path.exists(stamp_path(level)):
        os.remove(stamp_path(level))  # a stamp is published only after this cook saves
    assert levels.new_level(level, False), level
    world = editor.get_editor_world()
    ship = unreal.SCImportLibrary.import_folder(world, root, True, scene)
    if not ship:
        results.append('%s import failed' % name)
        continue
    ship.set_actor_label('Ship ' + name)
    gear = unreal.SCImportLibrary.rest_on_floor(ship, VIEWER_FLOOR_Z)
    try:
        audio = cook_audio.stage(root, scene, level.rsplit('/', 1)[1])
    except cook_audio.CookAudioError as e:
        results.append('%s STALE EXPORT (%s)' % (name, e))
        unreal.log_warning('COOK_SHIP ' + results[-1])
        continue
    ship.set_editor_property('cooked_audio', audio[0])
    ship.set_editor_property('cooked_audio_scene', audio[1])
    ok = unreal.SCCookLibrary.cook_environment(ship, '/Game/SC/Ships/' + name, '/Game/SC/Ships/Shared')
    saved = levels.save_current_level()
    if ok and saved:
        with open(stamp_path(level), 'w', encoding='utf-8') as f:
            f.write('%d\n' % FORMAT)
    results.append('%s gear_clips=%d cook=%s level_saved=%s format=%d' % (name, gear, ok, saved, FORMAT))
    unreal.log('COOK_SHIP ' + results[-1])
unreal.log('COOK_DONE ' + ' | '.join(results))
