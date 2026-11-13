using UnrealBuildTool;
public class SCViewerEditorTarget : TargetRules
{
    public SCViewerEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        // Latest: V8 / Unreal5_9 on the 5.9 install, V7 / Unreal5_8 on the NvRTX 5.8 source build (Win64).
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.Add("SCViewer");
    }
}
