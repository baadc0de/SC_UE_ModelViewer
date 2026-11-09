using UnrealBuildTool;
public class SCViewerEditorTarget : TargetRules
{
    public SCViewerEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.V7;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
        ExtraModuleNames.Add("SCViewer");
    }
}
