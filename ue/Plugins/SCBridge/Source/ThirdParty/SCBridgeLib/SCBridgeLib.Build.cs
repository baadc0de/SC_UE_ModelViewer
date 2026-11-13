using System.IO;
using UnrealBuildTool;

// The Rust FFI library (crates/scbridge-ffi), staged by scripts/build_ffi.sh (Linux) or scripts/build_ffi.ps1 (Win64).
public class SCBridgeLib : ModuleRules
{
    public SCBridgeLib(ReadOnlyTargetRules Target) : base(Target)
    {
        Type = ModuleType.External;
        PublicSystemIncludePaths.Add(Path.Combine(ModuleDirectory, "include"));
        if (Target.Platform == UnrealTargetPlatform.Linux)
        {
            string Library = Path.Combine(ModuleDirectory, "lib", "Linux", "libscbridge.so");
            PublicAdditionalLibraries.Add(Library);
            PublicRuntimeLibraryPaths.Add(Path.GetDirectoryName(Library));
            PublicRuntimeLibraryPaths.Add("$(BinaryOutputDir)");
            RuntimeDependencies.Add("$(BinaryOutputDir)/libscbridge.so", Library);
        }
        else if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            // Rust's MSVC cdylib: scbridge.dll plus its import library scbridge.dll.lib. Implicitly linked; the DLL is
            // copied beside the module so the loader finds it in the binaries directory.
            string LibDir = Path.Combine(ModuleDirectory, "lib", "Win64");
            PublicAdditionalLibraries.Add(Path.Combine(LibDir, "scbridge.dll.lib"));
            RuntimeDependencies.Add("$(BinaryOutputDir)/scbridge.dll", Path.Combine(LibDir, "scbridge.dll"));
            if (File.Exists(Path.Combine(LibDir, "scbridge.pdb")))
            {
                RuntimeDependencies.Add("$(BinaryOutputDir)/scbridge.pdb", Path.Combine(LibDir, "scbridge.pdb"));
            }
        }
        else
        {
            throw new BuildException("SCBridgeLib supports Linux and Win64 only.");
        }
    }
}
