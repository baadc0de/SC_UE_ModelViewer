using UnrealBuildTool;
public class SCViewer : ModuleRules
{
    public SCViewer(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "SCBridge" });
    }
}
