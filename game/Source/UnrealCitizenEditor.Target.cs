using UnrealBuildTool;
public class UnrealCitizenEditorTarget : TargetRules
{
    public UnrealCitizenEditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        // Latest: the engine's own settings and include order (V8 / Unreal5_9 on UE 5.9).
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.AddRange(new[] { "UnrealCitizen", "UnrealCitizenEditor" });
    }
}
