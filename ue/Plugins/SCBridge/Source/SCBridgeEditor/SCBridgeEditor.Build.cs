using UnrealBuildTool;
public class SCBridgeEditor : ModuleRules
{
    public SCBridgeEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "UnrealEd", "SCBridge" });
    }
}
