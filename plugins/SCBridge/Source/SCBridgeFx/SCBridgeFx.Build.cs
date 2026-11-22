// SCBridgeFx: the editor-only builder of the game's particle effects as Niagara systems (WP-K4c, SCFxBuild commandlet).
using UnrealBuildTool;
public class SCBridgeFx : ModuleRules
{
    public SCBridgeFx(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = false;
        // Niagara's lightweight (stateless) emitter authoring types (the emitter, its spawn infos and emitter state, the
        // module classes and their settings) live in Niagara/Internal/Stateless, which UBT lets only engine modules include.
        // The fork exports what had no public form (the spawn-info capacity in a public header, the expression structs;
        // WP-E1 E1-6), but a public header set for the rest would move most of that tree (see the WP's Result (E1-6)), so
        // this small editor-only module keeps the flag and the rest of the plugin stays an ordinary game module.
        bTreatAsEngineModule = true;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "Json", "Projects", "RenderCore", "AssetRegistry", "UnrealEd",
            "Niagara", "NiagaraCore", "NiagaraShader", "XmlParser",
            // SCBridgeLib directly: the commandlet calls the FFI (sc_particle_effect, sc_p4k_read_dds, ...); Win64 DLLs
            // link the import library themselves (see SCBridgeEditor).
            "SCBridge", "SCBridgeLib",
        });
    }
}
