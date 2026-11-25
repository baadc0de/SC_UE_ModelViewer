using UnrealBuildTool;
public class SCViewerTarget : TargetRules
{
    public SCViewerTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        // Latest: the engine's own settings and include order (V8 / Unreal5_9 on UE 5.9).
        DefaultBuildSettings = BuildSettingsVersion.Latest;
        IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
        ExtraModuleNames.Add("SCViewer");
        // The public release is Shipping (viewer/scripts/package_viewer.ps1 -Release). It keeps its log
        // (%LOCALAPPDATA%\SCViewer\Saved\Logs\SCViewer.log) so a problem report can carry it; a setting that differs
        // from the engine's shared build environment needs one of its own.
        BuildEnvironment = TargetBuildEnvironment.Unique;
        bUseLoggingInShipping = true;
        // NVIDIA DLSS stays out of the public release (its redistribution terms are not part of this repository's
        // licence); TSR is the anti-aliaser there. Development packages keep it when the plugin is installed.
        if (Target.Configuration == UnrealTargetConfiguration.Shipping) DisablePlugins.Add("DLSS");
    }
}
