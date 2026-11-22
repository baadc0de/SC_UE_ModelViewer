using UnrealBuildTool;
public class SCViewer : ModuleRules
{
    public SCViewer(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "SCBridge" });
        // Read at run time from Config (FSCCargoDroneArt, FSCCargoFx, the live-spawn default fleet): staged with a packaged build.
        RuntimeDependencies.Add("$(ProjectDir)/Config/Cargo/drone_art.json", StagedFileType.UFS);
        RuntimeDependencies.Add("$(ProjectDir)/Config/Cargo/fleet.json", StagedFileType.UFS);
        RuntimeDependencies.Add("$(ProjectDir)/Config/FX/effects.json", StagedFileType.UFS);
    }
}
