using UnrealBuildTool;
public class UnrealCitizenTarget : TargetRules
{
    public UnrealCitizenTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        // Latest: the engine's own settings and include order (V8 / Unreal5_9 on UE 5.9).
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.Add("UnrealCitizen");
    }
}
