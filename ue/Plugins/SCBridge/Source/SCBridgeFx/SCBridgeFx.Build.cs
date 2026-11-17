// SCBridgeFx: the editor-only builder of the game's particle effects as Niagara systems (WP-K4c, SCFxBuild commandlet).
using UnrealBuildTool;
public class SCBridgeFx : ModuleRules
{
    public SCBridgeFx(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bUseUnity = false;
        // Niagara's lightweight (stateless) emitter API lives in Niagara/Internal/Stateless, which UBT lets only engine
        // modules include. This small module carries the flag so the rest of the plugin stays an ordinary game module.
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
