using System.Linq;
using System.Reflection;
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
        if (Target.Platform.ToString() == "Wasm") MapPluginSourcePaths();
    }

    /// The web build's release scan (UE 5.9 Wasm deploy.py) refuses a wasm that carries build-machine paths; the Wasm
    /// toolchain maps the engine and project folders (-ffile-prefix-map), but our plugins live beside the project
    /// (AdditionalPluginDirectories ../Plugins), so their __FILE__ paths stay absolute. Map that folder to /Plugins too.
    /// Through reflection: WasmPlatform exists only where the engine has the Wasm platform (not every Linux install).
    void MapPluginSourcePaths()
    {
        if (ProjectFile == null) return;
        object Wasm = GetType().GetField("WasmPlatform")?.GetValue(this) ?? GetType().GetProperty("WasmPlatform")?.GetValue(this);
        FieldInfo Extra = Wasm?.GetType().GetField("ExtraCompileArgs");
        if (Extra == null) return;
        string Plugins = System.IO.Path.GetFullPath(System.IO.Path.Combine(ProjectFile.Directory.FullName, "..", "Plugins")).TrimEnd('\\', '/');
        string Maps = string.Join(" ", new[] { Plugins, Plugins.Replace('\\', '/') }.Distinct().Select(From => $"\"-ffile-prefix-map={From}=/Plugins\""));
        string Existing = (string)Extra.GetValue(Wasm) ?? "";
        Extra.SetValue(Wasm, string.IsNullOrWhiteSpace(Existing) ? Maps : Existing + " " + Maps);
    }
}
