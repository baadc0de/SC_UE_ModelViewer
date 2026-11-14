using UnrealBuildTool;
public class SCViewerTarget : TargetRules
{
    public SCViewerTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        // Latest: the installed engine's own settings and include order (V8 / Unreal5_9 on UE 5.9).
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.Add("SCViewer");
    }
}
