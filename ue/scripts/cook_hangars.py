"""WP-C1: cook exported hangars into levels (one per hangar) with pooled meshes/textures.

Run after scripts/export_hangars.sh, with a renderer (runtime textures and Nanite builds):
  UnrealEditor-Cmd SCViewer.uproject -run=pythonscript -script=<abs>/cook_hangars.py -vulkan -AllowCommandletRendering \
      -unattended -nop4 -ddc=SCViewerDDC -SCCookRoot=<abs>/out/hangars [-SCCookOnly=hightech_a_hangar_lrgtop_001]
Levels: /Game/SC/Hangars/Maps/<style>_<hangar>; materials: /Game/SC/Hangars/<style>_<hangar>/Materials;
meshes and textures: /Game/SC/Hangars/Shared. All game-derived (gitignored). Cooked hangars are skipped: to re-cook
one, delete Content/SC/Hangars/Maps/<name>.umap and Content/SC/Hangars/<name>/ first (the shared pool stays).
"""
import os
import re
import unreal

args = unreal.SystemLibrary.get_command_line()
root = re.search(r'-SCCookRoot=(\S+)', args).group(1)
only = re.search(r'-SCCookOnly=(\S+)', args)
only = set(only.group(1).split(',')) if only else None
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
    name = '%s_hangar_%s_%s' % (style, m.group(2), m.group(3))
    if only and name not in only:
        continue
    level = '/Game/SC/Hangars/Maps/' + name
    if unreal.EditorAssetLibrary.does_asset_exist(level):
        # The asset registry scanned it at startup: delete Content/SC/Hangars/Maps/<name>.umap and
        # Content/SC/Hangars/<name>/ before launching to re-cook (the shared pool stays).
        results.append('%s already cooked (skipped)' % name)
        unreal.log('COOK_HANGAR ' + results[-1])
        continue
    assert levels.new_level(level, False), level
    world = editor.get_editor_world()
    env = unreal.SCImportLibrary.import_environment(world, root, 'Packages/%s/scene.json' % package)
    if not env:
        results.append('%s import failed' % name)
        continue
    env.set_actor_label('Environment ' + name)
    offset = unreal.SCImportLibrary.present_environment(env, 'open', -5.0)
    ok = unreal.SCCookLibrary.cook_environment(env, '/Game/SC/Hangars/' + name)
    saved = levels.save_current_level()
    results.append('%s offset=(%.1f, %.1f, %.1f) cook=%s level_saved=%s' % (name, offset.x, offset.y, offset.z, ok, saved))
    unreal.log('COOK_HANGAR ' + results[-1])
unreal.log('COOK_DONE ' + ' | '.join(results))
