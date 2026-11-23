using UnrealBuildTool;
public class UnrealCitizenEditor : ModuleRules
{
    public UnrealCitizenEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        // File-local helpers in anonymous namespaces (SafeName differs per file): no unity batching (see SCBridge).
        bUseUnity = false;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "InterchangePipelines" });
        // SCBridgeLib directly: SCCookLibrary calls the FFI (sc_texels_decode_bgra8); Win64 DLLs must link the import
        // library themselves (Linux resolved the symbols through SCBridge's shared object at load time).
        PrivateDependencyModuleNames.AddRange(new[] { "UnrealEd", "SCBridge", "SCBridgeLib", "UnrealCitizen", "ToolMenus", "DesktopPlatform", "Slate", "SlateCore", "InterchangeEngine", "InterchangeCore", "InterchangeNodes", "InterchangeFactoryNodes", "StaticMeshDescription", "MeshDescription", "RenderCore", "RHI", "AssetRegistry", "LevelEditor", "Json", "Chaos", "PhysicsCore" });
    }
}
