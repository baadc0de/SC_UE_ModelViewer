using UnrealBuildTool;
public class SCBridgeEditor : ModuleRules
{
    public SCBridgeEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "InterchangePipelines" });
        PrivateDependencyModuleNames.AddRange(new[] { "UnrealEd", "SCBridge", "ToolMenus", "DesktopPlatform", "Slate", "SlateCore", "InterchangeEngine", "InterchangeCore", "InterchangeNodes", "InterchangeFactoryNodes", "StaticMeshDescription", "MeshDescription", "RenderCore", "RHI" });
    }
}
