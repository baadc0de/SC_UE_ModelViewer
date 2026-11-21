# WP-M4 · Thruster previews in Ship Link

Ship Link's Systems panel gives the selected ship four visual states. Off clears engine glow and stops the exhaust systems. Minimum runs the authored thrust effect at the low end of its driver range. Full uses the high end for main thrust. Boosted uses authored afterburners on main thrusters and keeps other mains at full normal thrust. Manoeuvring and retro thrusters stay at their authored low end in both forward-thrust previews. These controls change the rendered exhaust, driven materials and available nozzle poses. The vehicle's movement and power systems stay independent.

## Source and package

The decomposed ship exporter identifies thruster item classes and reads their SEntityComponentEffects particle and material categories. It resolves the effect and parameter GUIDs through DataCore's Tag records. The particle references, host attachment names, offsets and strength modifiers are saved on each relevant child as thruster_vfx. The importer attaches the Niagara system to the authored rig bone or static helper. Nozzle targets come from SCItemThrusterParams.nozzleAnimation and match a clip or fragment on that thruster's own rig. On source items with an empty nozzle field, the effect and emissive state still work.

The main C2 thruster is a checked source example. Its Thrust and AfterBurn tags point at different particle systems on the same exhaust helper. The Cutter supplies another set of thrust and boost effects and a nozzle fragment reference. Damage, misfire, trails and atmospheric contrails have separate source tags.

## Nozzle preview diagnosis

Opus 5.5 checked the extracted records, Niagara bake and prior captures. The active references are authored
Thrust and AfterBurn effects on the nozzle helpers. They include flume, afterburner sparks and local glow, and
the source treats flight trails as separate effects. The earlier preview raised all outlets together, including
opposing manoeuvring and retro thrusters. The forward-thrust preview now uses the authored item type to leave
those outlets at idle. The diagnostic line reports main and manoeuvring strengths independently.

The Niagara bake still samples strength-driven size and lifetime at full strength. Matching the game's
low-thrust plume shape needs a separate renderer change and a visual check.

## Exhaust orientation and sprite dimensions

The thruster placement composes the particle offset and axes with the authored helper or bone in its
source frame. That removes a 90° mismatch introduced by the decomposed GLB wrapper. The Niagara bake
honors the sign of emitter speed, so negative speed travels opposite the helper's positive axis and a
mirrored authoring setup can use positive speed. It also applies authored acceleration. The viewer uses
emitter space for the acceleration on layers marked `MoveRelativeEmitter`, based on the Cutter exhaust
records and the stationary preview. World-space acceleration is used on other layers.

Sprite dimensions use each flipbook cell's pixel proportions before applying the authored aspect. The
Cutter's 128×512 outer-flame texture therefore has one quarter of its previous sprite width. Color textures
decoded from BC7 bake back into BC7, which avoids the particle baker's previous conversion to BC1 or BC3.
The legacy basis for the distance and aspect units is documented in the
[CryEngine particle parameter reference](https://www.cryengine.com/docs/static/engines/cryengine-3/categories/1114113/pages/11239648).
The per-frame aspect mapping follows
[Lumberyard's texture-frame calculation](https://raw.githubusercontent.com/aws/lumberyard/master/dev/Code/CryEngine/Cry3DEngine/ParticleEffect.cpp)
and [sprite vertex scaling](https://raw.githubusercontent.com/aws/lumberyard/master/dev/Code/CryEngine/Cry3DEngine/ParticleRender.cpp).

The generated FX contract is now version 4. Rebuild the configured cargo and impact effects with
`-run=SCFxBuild`, then rebuild any local ship exhaust systems with the references below. Check the local
packages with `-run=SCFxBuild -VerifyContract`.

For the visual check, capture fixed aft and profile views with the normal project look in all four states.
Thrust should travel outward along each nozzle. Boost should read as separate tapered jets with the hot
centers inside the mouths and the housing legible. The Cutter's supplied game stills establish the visual
target. The C2 in the XL hangar serves as a second check of the attachment axes and mode changes.

The Win64 Development Editor build and all ten FX and mesh-basis automation tests passed. The
commandlet rebuilt the 16 Cutter and C2 systems and ran the ten configured cargo and impact bake
entries with zero errors. The contract verifier found 22 unique systems, with zero stale packages
or load failures. In serialized D3D12 Surface Cache runs, both ships resolved every exhaust binding
through all four modes. Mount logs showed the signed launch axes point aft on the forward thrusters
of both ships. The captures and logs are in the ignored `out/m4-nozzle-preview/frame-aspect` folder.

Astra confirmed the direction correction on both ships. The Cutter's small jet has a narrow pink
tail in profile, and the new streaks leave more of the housing visible. The large blue fill still
stays too thick downstream. The large afterburn core still adds an oversized round halo at boost,
and Minimum can leave detached dashes around the rim. A diagnostic capture of each layer separately
identified the blue `Core_Fill`, afterburn cores and outer streaks as separate contributors. Their
footprint and opacity remain open under the reference-based visual criterion.

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

The follow-up Win64 build and serialized D3D12 replays completed for the Cutter and C2. In Full, the main
strength sums were 4 and 12. Manoeuvring strength stayed at zero across 16 Cutter and 18 C2 emitter
bindings. Boost selected 4 and 8 authored afterburners. All systems resolved, both Off passes left zero active
systems, and neither ship moved. These checks establish the mode split. The short-plume look still needs a
matched visual check after the renderer supports the authored size and lifetime response.
