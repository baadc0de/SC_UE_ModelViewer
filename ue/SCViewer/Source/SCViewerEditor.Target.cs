using UnrealBuildTool;
public class SCViewerEditorTarget : TargetRules
{
    public SCViewerEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.V8;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_9;
        ExtraModuleNames.Add("SCViewer");
    }
}
