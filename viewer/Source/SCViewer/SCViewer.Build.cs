using UnrealBuildTool;
public class SCViewer : ModuleRules
{
    public SCViewer(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "SCBridge" });
        PrivateDependencyModuleNames.AddRange(new[] { "Slate", "SlateCore", "InputCore", "AppFramework" }); // AppFramework: the colour picker
        // Read at run time from Config (the game's effects, SCFxRules): staged with a packaged build.
        RuntimeDependencies.Add("$(ProjectDir)/Config/FX/effects.json", StagedFileType.UFS);
    }
}
