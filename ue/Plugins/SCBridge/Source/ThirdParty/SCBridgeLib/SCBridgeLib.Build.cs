using System.IO;
using UnrealBuildTool;

// The Rust FFI library (crates/scbridge-ffi), staged by scripts/build_ffi.sh (Linux) or scripts/build_ffi.ps1 (Win64).
// Other platforms (Wasm: the baked web build) have no library: SC_WITH_FFI=0 and SCBridge's SCBridgeNoFFI.cpp
// implements the C API as "unsupported", so Data.p4k is never read there.
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
            PublicDefinitions.Add("SC_WITH_FFI=1");
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
            PublicDefinitions.Add("SC_WITH_FFI=1");
        }
        else
        {
            PublicDefinitions.Add("SC_WITH_FFI=0");
        }
    }
}
