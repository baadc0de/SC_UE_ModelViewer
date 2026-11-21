# WP-M4 · Thruster previews in Ship Link

Ship Link's Systems panel gives the selected ship four visual states. Off clears engine glow and stops the exhaust systems. Minimum runs the authored thrust effect at the low end of its driver range. Full uses the high end. Boosted uses the authored afterburner effect where the thruster has one and keeps the normal full effect on other thrusters. These controls change the rendered exhaust, driven materials and available nozzle poses. The vehicle's movement and power systems stay independent.

## Source and package

The decomposed ship exporter identifies thruster item classes and reads their SEntityComponentEffects particle and material categories. It resolves the effect and parameter GUIDs through DataCore's Tag records. The particle references, host attachment names, offsets and strength modifiers are saved on each relevant child as thruster_vfx. The importer attaches the Niagara system to the authored rig bone or static helper. Nozzle targets come from SCItemThrusterParams.nozzleAnimation and match a clip or fragment on that thruster's own rig. On source items with an empty nozzle field, the effect and emissive state still work.

The main C2 thruster is a checked source example. Its Thrust and AfterBurn tags point at different particle systems on the same exhaust helper. The Cutter supplies another set of thrust and boost effects and a nozzle fragment reference. Damage, misfire, trails and atmospheric contrails have separate source tags.

## Preparing local exhaust assets

Export each ship with attachments. The first entry in the resulting tar is the scene manifest and contains the authored references. The helper reads just that entry. Pass the unique references to the existing SCFxBuild commandlet to generate the local Niagara assets.

~~~powershell
$refs = @(./ue/scripts/thruster_fx_refs.ps1 -Package $packageTars)
& $editorCmd $uproject '-run=SCFxBuild' "-Effects=$($refs -join ';')" '-unattended' '-NullRHI'
~~~

The viewer reads the resulting Niagara assets under /Game/SC/FX through the existing FX contract. A missing asset is reported in Ship Link. Ship packages from before this work can be re-exported through the normal load path. Game media and generated Niagara assets stay out of source control.

## Verification

The focused Rust test checked tag lookup, disabled-effect exclusion, attachment offsets and modifier ordering. Win64 SCViewerEditor built successfully. SCFxBuild converted seven Cutter and nine C2 effect references with zero build errors.

In serialized D3D12 sessions, the Cutter bound 20 exhaust systems and posed two authored nozzles. Boost selected four alternative effects. The C2 bound all 28 thruster placements in the XL hangar, including wildcard retro exhaust helpers, and ran 30 systems. Boost selected eight alternative effects. Off left zero active exhaust systems in both ships. Both ships kept exactly the same world position and rotation across the five mode changes.

Astra checked the matched exposures. The visible C2 upper plumes project away from their outlets and extend farther under boost. The Cutter's small orange exhaust was also visible in profile. Bright boost near the Cutter's largest outlet obscured part of the nozzle, and the captures cover a subset of the other outlets.
