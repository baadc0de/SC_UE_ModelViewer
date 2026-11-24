using UnrealBuildTool;
// WP-O1-4c: the importer (archive client, package reading, import pipeline, processed caches, Ogg decoder) and the
// scbridge library it calls. The viewer and the game's editor load it; a packaged game (Game target) does not.
public class SCBridgeImport : ModuleRules
{
    public SCBridgeImport(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        // File-local helpers per .cpp (anonymous namespaces); unity batches would merge them (SCBridge does the same).
        bUseUnity = false;
        // SCBridgeLib is public: SCBridgeClient.h exposes sc_options from scbridge.h.
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "MeshDescription", "SCBridge", "SCBridgeLib" });
        PrivateDependencyModuleNames.AddRange(new[] { "Json", "StaticMeshDescription", "RenderCore", "RHI", "PhysicsCore", "Chaos", "ChaosCore",
            "GeometryCore", "ImageWrapper", "Projects", "Slate", "SlateCore", "InputCore", "ApplicationCore", "Niagara", "NiagaraShader", "XmlParser",
            "AudioMixer", "AudioExtensions", "SignalProcessing", "Synthesis", "AssetRegistry" });
        if (Target.bBuildEditor)
            PrivateDependencyModuleNames.AddRange(new[] { "TargetPlatform", "NaniteBuilder", "MeshBuilder", "MeshUtilities" });
    }
}
