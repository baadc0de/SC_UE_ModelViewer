# Cargo data: Star Citizen LIVE 4.10.2

Read-only investigation of the auto-discovered LIVE Data.p4k, using the existing debug binaries. Coordinates and dimensions below are source x/y/z; dimensions are metres where represented by inventory vectors. GUIDs identify DataCore records unless explicitly called instance GUIDs. Derived capacities are geometric volumes, not evidence of an independently authored capacity, packing rules, or a live-game limit. Full query results and exported package are scratch under `out/cargo-datamine/`; they are not repository artifacts.

## Hull A

`entity loadout MISC_Hull_A` is a substring query; use the first `EntityClassDefinition.MISC_Hull_A` root (GUID `6ec72f64-2002-481c-b7c9-b132476ba6aa`), not its AI variants. It resolves four occurrences:

```text
MISC_Hull_A_CargoPlate [hardpoint_cargoplate_bot_left]  -> MISC_Hull_A_CargoGrid [Hardpoint_Cargo] geom=-
MISC_Hull_A_CargoPlate [hardpoint_cargoplate_top_left]  -> MISC_Hull_A_CargoGrid [Hardpoint_Cargo] geom=-
MISC_Hull_A_CargoPlate [hardpoint_cargoplate_bot_right] -> MISC_Hull_A_CargoGrid [Hardpoint_Cargo] geom=-
MISC_Hull_A_CargoPlate [hardpoint_cargoplate_top_right] -> MISC_Hull_A_CargoGrid [Hardpoint_Cargo] geom=-
```

| Record | GUID | Source and findings |
|---|---|---|
| `MISC_Hull_A_CargoPlate` | `3f73dc4b-abc5-448b-a449-f02627697d4a` | `Components[SGeometryResourceParams].Geometry.Geometry.Geometry.path`: `Objects/Spaceships/Ships/MISC/Hull_A/exterior/Cargo_Plate_Arms/MISC_Hull_A_Skinned_Cargo_Plates.cdf`. Attachable type `CargoGrid`; its loadout holds the grid on `Hardpoint_Cargo`. |
| `MISC_Hull_A_CargoGrid` | `14978620-d1df-4aa4-92c6-9110a06a4823` | `Components[SCItemInventoryContainerComponentParams].containerParams` -> `libs/foundry/records/inventorycontainers/cargogrid/misc_hull_a_cargogrid.json`; no geometry in loadout. |
| `InventoryContainer.MISC_Hull_A_CargoGrid` | `1c48687e-fe7d-487c-ac3b-f1e98414917c` | `interiorDimensions=(2.5,5,2.5)`; `inventoryType` is `InventoryOpenContainerType`; `gridCellSize.centimeters=1`; `minPermittedItemSize=(1.25,1.25,1.25)`, `maxPermittedItemSize=(2.5,5,2.5)`, `isExternalContainer=true`, `gridPosOffset=(0,0,0)`, `excludedItemSubTypes=[]`. |

The 1-SCU inventory box independently supplies `SStandardCargoUnit.standardCargoUnits=1` and `SItemDefinition.inventoryOccupancyDimensions=(1.25,1.25,1.25)`. Thus the requested 1.25 m cube is supported for that box; it is **not** the grid's authored placement step, which is 1 cm. Dividing each axis gives 2 × 4 × 2 nominal SCU cubes, 16 per grid, 64 across four, purely by volume. Arbitrary object acceptance and fit cannot be decided from nominal volume alone.

The plate has `SEntityInteractableParams.Interactable.SharedInteractions` named `StrutTransformExtend` and `StrutTransformRetract`. Cargo-plate skin, rotator and extender bones and deploy animation are present. Archive files include `Data/Animations/Mannequin/ADB/SCItems/Cargo/MISC_Hull_A_Cargo/MISC_Hull_A_Cargo_Anims.adb` and `Data/Objects/Spaceships/Ships/MISC/Hull_A/exterior/Cargo_Plate_Arms/MISC_Hull_A_Cargo_Plates_CHR.chrparams`. Exported clips include `misc_hull_a_cargo_deploy`, and plate `Misc Hull A Cargo Deploy`, `Deployed`, `Retracted`; animation tracks include `hardpoint_cargo_grid`, `BONE_cargoplate_rotator`, and `BONE_cargoplate_extender`. The hull has animated plate hardpoints too. These establish movable plate/spine mechanisms, but exact grid orientation and world transforms in either pose were not resolved: `Hardpoint_Cargo` versus animated `hardpoint_cargo_grid` needs helper/bone mapping and transform composition verification. Port labels do not establish axis orientation.

## Boxes and commodities

The following records have `Components[SAttachableComponentParams].AttachDef.inventoryOccupancyVolume` and `inventoryOccupancyDimensions`; `Components[SEntityPhysicsControllerParams].PhysType.Mass` supplies the raw mass. `Components[SGeometryResourceParams].Geometry.Geometry.Geometry.path` supplies geometry. Common prefix for the geometry column is `Objects/buildingsets/human/lowtech/prop/storage/cargo/cargo_multipurpose_1_`; suffix `.cgf`. Empty `Geometry.Geometry.Material.path` and `Material.Material.path` mean no DataCore material override was specified in these records; actual mesh material was not resolved. The two smallest examples are commodity-specific and the other five are organic category records; these masses must not be generalized to all commodities.

| SCU | Entity `EntityClassDefinition.` | GUID | Dimensions x/y/z | Mass (raw) | Geometry suffix |
|---:|---|---|---|---:|---|
| 1 | `Carryable_TBO_FL_1SCU_Commodity_Organic_FreshFood` | `d5f599b5-dfca-4121-a1c9-476948b8310b` | 1.25/1.25/1.25 | 938 | `0125x0125x0125_a` |
| 2 | `Carryable_TBO_FL_2SCU_Commodity_Organic_Fotia` | `4a374205-8195-4601-83ad-1bd59ff400d2` | 1.25/2.5/1.25 | 1875 | `025x0125x0125_a` |
| 4 | `Carryable_TBO_FL_4SCU_Commodity_Organic` | `0cade6a6-2bca-4403-b67e-aedb1e5e804f` | 2.5/2.5/1.25 | 3750 | `025x025x0125_a` |
| 8 | `Carryable_TBO_FL_8SCU_Commodity_Organic` | `392d7361-4620-4d61-99f7-dfb58408fd7f` | 2.5/2.5/2.5 | 7500 | `025x025x025_a` |
| 16 | `Carryable_TBO_FL_16SCU_Commodity_Organic` | `1df43415-81ff-4fca-9432-465da8f3d9c0` | 2.5/5/2.5 | 15000 | `025x025x05_a` |
| 24 | `Carryable_TBO_FL_24SCU_Commodity_Organic` | `3c66ed82-ce0b-49b2-866f-d315765b42a7` | 2.5/7.5/2.5 | 26250 | `025x025x075_a` |
| 32 | `Carryable_TBO_FL_32SCU_Commodity_Organic` | `06ab17e1-cb9e-4455-87a3-9492e04d8c94` | 2.5/10/2.5 | 30000 | `025x025x1_a` |

Separate physical inventory boxes `Carryable_TBO_InventoryContainer_{1,2,4,8}SCU` have the same corresponding dimensions/geometry, but raw masses 600/1200/2400/4800; GUIDs respectively `44512a35-3a37-48b9-bf0e-c1bf87b4503b`, `d6f1b600-353d-42d7-a592-450172889f00`, `ced4ab55-c8d0-42d9-86d9-71f97eb38872`, `64d1c96f-a7c4-492f-a12e-91699e3b01bc`. There are also `Carryable_2H_FL_EighthSCU_Commodity_*` records. Commodity families and inventory boxes are credible freight-grid candidates, but a direct freight-elevator spawn/family selection rule was not established; do not infer one canonical family from matching meshes. `SCU_Cargo_Template_*` records are not a reliable substitute: e.g. 1-SCU template `d98af276-fc5c-4b93-aa40-be3db0865691` has default 0.15 m occupancy and one microSCU despite its name.

## Freight elevators

Every grid entity points via `SCItemInventoryContainerComponentParams.containerParams` into `libs/foundry/records/inventorycontainers/cargogrid/freightelevator_cargogrid_<size>.json`. The following are open containers with 1 cm `gridCellSize`, zero `gridPosOffset`, `isExternalContainer=false`, and minimum item (1.25,1.25,1.25). Capacity column divides authored dimensions by the observed 1-SCU box; no explicit capacity is claimed.

| Entity suffix / entity GUID | `InventoryContainer.FreightElevator_CargoGrid_` suffix / record GUID | Dimensions m | Nominal SCU cells / volume | Max item m |
|---|---|---|---|---|
| `Hangar_FreightElevator_CargoGrid_Small` / `004baa98-099f-4276-921a-d07ea4646c09` | `Small` / `2b4f7aa5-baab-4cd6-a1ac-38cf6644dd7d` | 7.5/6.25/5 | 6/5/4 = 120 | 2.5/5/2.5 |
| `Hangar_FreightElevator_CargoGrid_Medium` / `2469d28f-1775-4584-acda-3097ed11d0de` | `Medium` / `8a49676c-2988-48b3-a6bf-a4884b711b59` | 10/8.75/5 | 8/7/4 = 224 | 2.5/10/2.5 |
| `Hangar_FreightElevator_CargoGrid_Large` / `4ec7d758-58e1-499b-ae1d-2515127a63d6` | `Large` / `4abec8a1-a8df-4482-84b6-52c16983ea84` | 20/17.5/5 | 16/14/4 = 896 | 2.5/10/2.5 |
| `Hangar_FreightElevator_CargoGrid_XL` / `8465c434-90fc-493e-9aec-a89465420601` | `XL` / `2e3aa693-8993-4a9b-aad0-4d68f0a931cd` | 25/17.5/5 | 20/14/4 = 1120 | 2.5/10/2.5 |
| `Exterior_FreightElevator_CargoGrid` / `52b05758-e01a-492a-adbb-7979a62b0b86` | `Exterior` / `096727f8-7378-40f6-aba3-e75b4b424707` | 40/25/10 | 32/20/8 = 5120 | 0/0/0 (semantics not established) |

Concrete placement chain, verified by extracting the socpaks (paths in `Data/ObjectContainers/`): `PU/loc/mod/common/hangar/hightech_a/hangar_smltop_001.socpak` -> child `.../hightech_a/hangar_component_module_cargo_elevator_sml.socpak` -> `setup/elevator_setup/elev_hightech/elev_ht_cargo_hangar_sml_a.socpak`. The last editor XML identifies `Hangar_FreightElevator_CargoGrid_Small_HT` instance `44de0e1e-22a9-08de-8731-2bb1f4b12890`, class `Hangar_FreightElevator_CargoGrid_Small`, and `LoadingPlatform` instance `455d9687-faf7-9f5a-18e2-b89392d06892`. The middle SOC contains `LoadingPlatformManager` class GUID `867b7b28-e62f-49dd-ae39-9057953e8e7b`, `SCLoadingPlatformManagerParams.loadingPlatformEntityReferences` linking platform, gate, kiosk and `CargoGrid`, and `LoadingPlatformFrontGate` class GUID `323fe170-8a3d-4d27-9b0f-6ab46345ba04`. Nested placement/animation must be composed to locate the grid. This proves this hangar's connection; other hangars were not exhaustively mapped.

High-tech kiosk `SoftLock_Terminal_Standard_HighTech_FreightElevatorKiosk_1_a` (`8f0531f0-5b2e-4802-9c29-b4e54a1453de`) geometry is `Objects/buildingsets/human/hightech/prop/interactive/console/console_cargo_elevator_1_a.cgf`; low-tech counterpart `b266dc9a-d532-4d3e-9c57-30a88b49e97a` uses `Objects/buildingsets/human/lowtech/prop/interactive/kiosk/kiosk_freight_1_a.cgf`. The archive also contains a util_a `hangar_smltop_001.socpak`, not inspected for its grid. Personal/passenger elevators have distinct `TransportCarriage_*_Elevator_*` classes; their semantics and grid absence were not verified. Platform geometry and all other hangar variants remain to be traced.

## Drone and tractor references

`dcb query EntityClassDefinition --filter '*Drone*'` returned 21 records, largely seats, doors and dashboards; it did not establish a small autonomous cargo drone entity. The P4k contains candidate **geometry only**, with no verified entity, dimensions or use: `Data/Objects/Vehicles/Repair/ph_repairdrone.cgf`, `Data/Objects/buildingsets/human/lowtech/prop/vehicle/mining/drone/mining_drone_body_001_a.cga` and `Data/Objects/Spaceships/Ships/AEGS/Reclaimer/Exterior/AEGS_Reclaimer_Drone.cga`. Do not assume these are all small, flyable or lift-capable.

| Entity and GUID | Evidence and tractor parameters (raw data values; units not established here) |
|---|---|
| `grin_multitool_01_tractorbeam` `03b3db91-0063-4da2-99f7-16b20ede4209` | Attachment geometry `Objects/fps_weapons/gadgets/grin/multitool/Gravity/grin_multitool_01_gravity.cdf`. |
| `grin_multitool_01_default_tractorbeam` `dc0b1117-8590-43c7-9c68-7644ed8c89aa` | Tool geometry `Objects/fps_weapons/gadgets/grin/multitool/gdgt_fps_grin_multitool.cdf`; `Components[].aimAction.conditionalWeaponActions[].weaponAction` includes separate tractor actions: one has minForce 500, maxForce 257500, maxVolume 20, maxDistance 150, fullStrengthDistance 3; another has 100000, 260000, 800, 25, 10 respectively. Their activation conditions must be interpreted before choosing one. |
| `WEP_TractorBeam_S1_Utility_1` `355b26c5-a3b7-4c34-ad8b-d281473715e8` | `Components[].fireActions[0]`: minForce 1500, maxForce 600000, maxVolume 300000, maxDistance 130, fullStrengthDistance 65. Geometry `Objects/Spaceships/Utilities/Tractor_Beams/GRIN/grin_tractor_beam_s1.cga`. |
| `GRIN_TractorBeam_S1` `34f8a503-41bc-4245-baea-e824c8a39411` | Same geometry; respective values 1500, 500000, 300000, 150, 75. |

Cargo-mode override parameters are separate from normal tractor values. No conclusion about gameplay balance or force units follows from these raw fields.

## Existing decomposed export

```text
> target/debug/scviewer.exe decomposed MISC_Hull_A out/cargo-datamine/hull_a --no-cache
996 files written ...; cache: miss
> rg 'entity_name.*CargoPlate|parent_node_name.*cargoplate|CargoGrid|interiorDimensions' <package>/scene.json
four MISC_Hull_A_CargoPlate items; four parent_node_name hardpoint_cargoplate_*; item_type CargoGrid
```

The four plate items have `local_transform_sc` identity, `offset_position/rotation` zero, parent hull and respective parent-node name, with skin mesh path and animation sidecars. Identity is the item's offset relative to its named attachment, **not** a world-space grid transform. No `MISC_Hull_A_CargoGrid` child, `Hardpoint_Cargo` item, container record reference, dimensions, min/max item sizes, cell resolution or external flag appear in scene.json. The plate's `item_type=CargoGrid` is insufficient to identify its geometry-less child or its inventory. The ship and plate animation tracks exist; this inspection did not establish whether their helper nodes alone are sufficient for a consumer to reconstruct all grid transforms. Export must carry the child and parent/helper link, local grid origin/orientation, source inventory parameters and pose-dependent attachment information before a client can know WxLxH and T.

## UI evidence

Both kiosk classes have `SFreightElevatorUIProviderParams.isItemBank=false` and a BuildingBlocks canvas reference `libs/foundry/records/ui/buildingblocks/i_interactivescreens/ter/i_ter_freightelevator_main.json`; audio reference `libs/foundry/records/audio/ui/buildingblocks/frieght_elevator_cargo_kiosk.json` retains the archive spelling. P4k has `Data/UI/Textures/I_InteractiveScreens/Ter/IC_Ter_FreightElevatorUI/` assets including `FreightElevator_32Crate_AttractScreen_Art.dds` and `ItemBankAttract_Grid_BG.dds`. The distinction between item bank and freight elevator is explicitly represented by the provider flag. `Data/UI/mobiGlas/assets/SWF/` includes `Economy.swf`, `ShipManagement.swf` and `LandingAreaServices.swf`; their existence does not prove a cargo ordering screen or fields. No verified mobiGlas cargo app record, manifest schema, or user-visible field list was recovered.

## Reproduction and evidence

```powershell
target/debug/starbreaker.exe entity loadout MISC_Hull_A
target/debug/starbreaker.exe dcb query EntityClassDefinition --filter '*MISC_Hull_A_Cargo*'
target/debug/starbreaker.exe dcb query InventoryContainer
target/debug/starbreaker.exe dcb query EntityClassDefinition --filter '*SCU*'
target/debug/starbreaker.exe dcb query EntityClassDefinition --filter '*Freight*'
target/debug/starbreaker.exe dcb query EntityClassDefinition --filter '*Drone*'
target/debug/starbreaker.exe dcb query EntityClassDefinition --filter '*Tractor*'
target/debug/starbreaker.exe dcb query EntityClassDefinition --filter '*tractorbeam*'
target/debug/starbreaker.exe p4k list --regex '(?i)(freight|hangar_smltop_001|hull_a.*cargo|cargo.*hull_a|tractorbeam)'
target/debug/starbreaker.exe p4k extract -o out/cargo-datamine/archive --filter '**/hangar_smltop_001.socpak'
target/debug/starbreaker.exe p4k extract -o out/cargo-datamine/archive --filter '**/hightech_a/hangar_component_module_cargo_elevator_sml.socpak'
target/debug/starbreaker.exe p4k extract -o out/cargo-datamine/archive --filter '**/elev_ht_cargo_hangar_sml_a.socpak'
target/debug/scviewer.exe decomposed MISC_Hull_A out/cargo-datamine/hull_a --no-cache
```

Trimmed `InventoryContainer` output, with the JSON paths above: `MISC_Hull_A_CargoGrid: interiorDimensions {x:2.5,y:5,z:2.5}; inventoryType { _Type_:InventoryOpenContainerType, gridCellSize:{centimeters:1}, minPermittedItemSize:{x:1.25,y:1.25,z:1.25}, maxPermittedItemSize:{x:2.5,y:5,z:2.5}, isExternalContainer:true }`. Socpak editor XML: `<Object name="Hangar_FreightElevator_CargoGrid_Small_HT" type="Hangar_FreightElevator_CargoGrid_Small" guid="44de0e1e-22a9-08de-8731-2bb1f4b12890" .../>`. The `InventoryContainer` record-name filter used by this CLI did not match lowercase file stems; querying the type and selecting record names produced the values above.

## Proposed generic export contract

Export **each actual container instance**, including geometry-less loadout children and socpak entities. Give stable instance ID, owner/entity and container record GUIDs and source paths. Preserve parent instance and attachment/helper or object-container placement chain, local affine transform and coordinate basis, grid origin/offset and whether it follows a bone/animated platform; expose or reference animation binding so the consumer composes the current pose. Do not silently bake a single default pose as a permanent transform.

Resolve `SCItemInventoryContainerComponentParams.containerParams` to `InventoryContainer`: retain `interiorDimensions`, concrete `inventoryType`, `gridCellSize` with units, `gridPosOffset`, min/max permitted dimensions, excluded subtypes, external flag and any explicit capacity or type restrictions, including zero values without assigning invented semantics. Distinguish authored centimetre placement resolution from an optional nominal SCU visualization, derived only from relevant source item dimensions and volume. For cargo candidates expose each item's record GUID, `SItemDefinition.inventoryOccupancyDimensions` and `inventoryOccupancyVolume`, item type/subtype/tags, geometry/material references and physics mass; preserve unrecognized restrictions and orientation rules rather than claiming all volume-fitting items are valid. A consumer can then compose owner -> animated helper -> grid transform and apply the data's constraints without ship-specific cases.
