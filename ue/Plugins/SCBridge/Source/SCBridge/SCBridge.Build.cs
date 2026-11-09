using UnrealBuildTool;
using System;
public class SCBridge : ModuleRules
{
    public SCBridge(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
        PrivateDependencyModuleNames.AddRange(new[] { "MeshDescription", "StaticMeshDescription", "RenderCore", "RHI", "PhysicsCore", "SCBridgeLib" });
        PublicDefinitions.Add("SC_SPIKE_RAW_SERIALIZE=" + (Environment.GetEnvironmentVariable("SC_SPIKE_RAW_SERIALIZE") == "1" ? "1" : "0"));
        if (Target.bBuildEditor)
            PrivateDependencyModuleNames.Add("TargetPlatform");
        // WP-U0 Spike A deliberately tests these dependencies in a Development Game target.
        if (Target.bBuildEditor || Target.Configuration == UnrealTargetConfiguration.Development)
            PrivateDependencyModuleNames.AddRange(new[] { "NaniteBuilder", "MeshBuilder" });
    }
}
