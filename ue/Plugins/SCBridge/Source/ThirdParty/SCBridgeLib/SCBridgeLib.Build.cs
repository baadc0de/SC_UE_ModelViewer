using System.IO;
using UnrealBuildTool;

public class SCBridgeLib : ModuleRules
{
    public SCBridgeLib(ReadOnlyTargetRules Target) : base(Target)
    {
        Type = ModuleType.External;
        if (Target.Platform != UnrealTargetPlatform.Linux)
        {
            throw new BuildException("SCBridgeLib currently supports Linux only.");
        }
        PublicSystemIncludePaths.Add(Path.Combine(ModuleDirectory, "include"));
        string Library = Path.Combine(ModuleDirectory, "lib", "Linux", "libscbridge.so");
        PublicAdditionalLibraries.Add(Library);
        PublicRuntimeLibraryPaths.Add(Path.GetDirectoryName(Library));
        PublicRuntimeLibraryPaths.Add("$(BinaryOutputDir)");
        RuntimeDependencies.Add("$(BinaryOutputDir)/libscbridge.so", Library);
    }
}
