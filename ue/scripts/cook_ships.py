"""Cook exported ships into levels (one per ship) with pooled meshes and textures. The viewer's LoadEntity streams
/Game/SC/Ships/Maps/<entity> instead of exporting from the archive, so a cooked ship also loads where there is no
archive (the web build, ue/scripts/package_web.ps1).

Export first (decomposed GLB + DDS into a shared root; the web build uses mip 1):
  scripts/starbreaker.sh entity export DRAK_Golem out/web/bake --kind decomposed --format glb --dds --lod 0 --mip 1 \
      --skip-existing-assets
then cook with a renderer (runtime textures and Nanite builds):
  UnrealEditor-Cmd SCViewer.uproject -run=pythonscript -script=<abs>/cook_ships.py -AllowCommandletRendering -unattended \
      -nop4 -SCCookRoot=<abs export root> [-SCCookOnly=DRAK_Golem,...]
Levels: /Game/SC/Ships/Maps/<entity>; materials: /Game/SC/Ships/<entity>/Materials; meshes and textures:
/Game/SC/Ships/Shared. All game-derived (gitignored). The ship is cooked as the viewer shows it: landing gear down,
resting on the viewer floor; rigs are baked static (CookEnvironment). Cooked ships are skipped: delete
Content/SC/Ships/Maps/<entity>.umap and Content/SC/Ships/<entity>/ to re-cook (the shared pool stays).
"""
import json
import os
import re

import unreal

VIEWER_FLOOR_Z = -5.0  # USCViewerSubsystem's floor: the cooked level streams in at the origin

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
        results.append('%s already cooked (skipped)' % name)
        unreal.log('COOK_SHIP ' + results[-1])
        continue
    assert levels.new_level(level, False), level
    world = editor.get_editor_world()
    ship = unreal.SCImportLibrary.import_folder(world, root, True, scene)
    if not ship:
        results.append('%s import failed' % name)
        continue
    ship.set_actor_label('Ship ' + name)
    gear = unreal.SCImportLibrary.rest_on_floor(ship, VIEWER_FLOOR_Z)
    ok = unreal.SCCookLibrary.cook_environment(ship, '/Game/SC/Ships/' + name, '/Game/SC/Ships/Shared')
    saved = levels.save_current_level()
    results.append('%s gear_clips=%d cook=%s level_saved=%s' % (name, gear, ok, saved))
    unreal.log('COOK_SHIP ' + results[-1])
unreal.log('COOK_DONE ' + ' | '.join(results))
