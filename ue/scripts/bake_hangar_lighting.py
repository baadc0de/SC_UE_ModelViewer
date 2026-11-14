"""Bake Lightmass lighting into a copy of a cooked hangar: /Game/SC/Hangars/Maps/<style>_<hangar>_Lit, which the viewer
streams instead of the dynamic one under -SCBakedLighting (the web build: its mobile renderer has no Lumen, so the
hangar's lights are baked into lightmaps and the volumetric lightmap; the ship stays movable and is lit from the latter).

Cook the hangar first (cook_hangars.py), then, with a renderer:
  UnrealEditor-Cmd SCViewer.uproject -run=pythonscript -script=<abs>/bake_hangar_lighting.py -AllowCommandletRendering \
      -unattended -nop4 -SCHangar=hightech_a_hangar_smltop_001 [-SCQuality=preview|medium|high|production] [-SCTexelCm=25]
The copy's components and lights become static (light functions dropped), its pooled meshes get generated lightmap UVs
(rebuilt and saved in place: the dynamic level renders as before), and lighting is built with reflection captures.
An existing _Lit level is replaced. Game-derived output (gitignored).
"""
import re

import unreal

args = unreal.SystemLibrary.get_command_line()
name = re.search(r'-SCHangar=(\S+)', args).group(1)
quality_arg = re.search(r'-SCQuality=(\w+)', args)
quality_name = quality_arg.group(1).lower() if quality_arg else 'preview'
texel_arg = re.search(r'-SCTexelCm=([\d.]+)', args)
texel_cm = float(texel_arg.group(1)) if texel_arg else 25.0
quality = {
    'preview': unreal.LightingBuildQuality.QUALITY_PREVIEW,
    'medium': unreal.LightingBuildQuality.QUALITY_MEDIUM,
    'high': unreal.LightingBuildQuality.QUALITY_HIGH,
    'production': unreal.LightingBuildQuality.QUALITY_PRODUCTION,
}[quality_name]

source = '/Game/SC/Hangars/Maps/' + name
target = source + '_Lit'
assets = unreal.EditorAssetLibrary
levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
assert assets.does_asset_exist(source), source + ' is not cooked (cook_hangars.py)'
for stale in (target, target + '_BuiltData'):
    if assets.does_asset_exist(stale):
        assert assets.delete_asset(stale), stale
assert levels.load_level(source), source
assert unreal.EditorLoadingAndSavingUtils.save_map(editor.get_editor_world(), target), target
assert levels.load_level(target), target
world = editor.get_editor_world()
environments = [a for a in unreal.GameplayStatics.get_all_actors_of_class(world, unreal.SCVehicle)]
assert len(environments) == 1, 'expected one environment actor, found %d' % len(environments)
prepared = unreal.SCCookLibrary.prepare_static_lighting(environments[0], texel_cm, 32, 1024)
unreal.log('BAKE_PREPARED ' + prepared)
built = levels.build_light_maps(quality, True)
saved = levels.save_current_level()
unreal.log('BAKE_DONE level=%s quality=%s texel_cm=%.0f built=%s saved=%s' % (target, quality_name, texel_cm, built, saved))
