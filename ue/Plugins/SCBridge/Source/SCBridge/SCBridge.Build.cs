using UnrealBuildTool;
using System;
public class SCBridge : ModuleRules
{
    public SCBridge(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "MeshDescription" });
        PrivateDependencyModuleNames.AddRange(new[] { "Json", "StaticMeshDescription", "RenderCore", "RHI", "PhysicsCore", "Chaos", "GeometryCore", "ImageWrapper", "Projects", "SCBridgeLib" });
        PublicDefinitions.Add("SC_SPIKE=" + (Environment.GetEnvironmentVariable("SC_SPIKE") == "1" ? "1" : "0"));
        PublicDefinitions.Add("SC_SPIKE_RAW_SERIALIZE=" + (Environment.GetEnvironmentVariable("SC_SPIKE_RAW_SERIALIZE") == "1" ? "1" : "0"));
        if (Target.bBuildEditor)
            PrivateDependencyModuleNames.AddRange(new[] { "TargetPlatform", "NaniteBuilder", "MeshBuilder", "MeshUtilities" });
    }
}
