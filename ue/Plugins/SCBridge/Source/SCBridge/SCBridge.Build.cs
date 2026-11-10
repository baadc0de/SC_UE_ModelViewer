using UnrealBuildTool;
using System;
public class SCBridge : ModuleRules
{
    public SCBridge(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        // SCBridgeLib is public: SCBridgeClient.h exposes sc_options from scbridge.h.
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "MeshDescription", "SCBridgeLib" });
        // Extracted Star Engine render constants (FSCStarEngine); staged with the plugin.
        RuntimeDependencies.Add("$(PluginDir)/Resources/StarEngineRender.json");
        PrivateDependencyModuleNames.AddRange(new[] { "Json", "StaticMeshDescription", "RenderCore", "RHI", "PhysicsCore", "Chaos", "GeometryCore", "ImageWrapper", "Projects", "Slate", "SlateCore", "InputCore", "ApplicationCore" });
        PublicDefinitions.Add("SC_SPIKE=" + (Environment.GetEnvironmentVariable("SC_SPIKE") == "1" ? "1" : "0"));
        PublicDefinitions.Add("SC_SPIKE_RAW_SERIALIZE=" + (Environment.GetEnvironmentVariable("SC_SPIKE_RAW_SERIALIZE") == "1" ? "1" : "0"));
        if (Target.bBuildEditor)
            PrivateDependencyModuleNames.AddRange(new[] { "TargetPlatform", "NaniteBuilder", "MeshBuilder", "MeshUtilities" });
        // Native p4k picker (WP-V1 startup screen): DesktopPlatform is a Developer module, absent from Shipping.
        bool bDesktopPlatform = Target.bBuildDeveloperTools;
        if (bDesktopPlatform) PrivateDependencyModuleNames.Add("DesktopPlatform");
        PrivateDefinitions.Add("SC_WITH_DESKTOP_PLATFORM=" + (bDesktopPlatform ? "1" : "0"));
    }
}
