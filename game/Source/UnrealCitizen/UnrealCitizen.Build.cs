using UnrealBuildTool;
// WP-O1-2: the game's own code on top of the shared SCBridge plugin: cargo operations (drones, jobs, beacons, scenarios),
// the mobiGlas and its apps (Cargo, Ship Link, Garage, Music) with the world callouts, and their RPC routes.
public class UnrealCitizen : ModuleRules
{
    public UnrealCitizen(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        // File-local helpers in anonymous namespaces, as in SCBridge: no unity batching.
        bUseUnity = false;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "SCBridge" });
        PrivateDependencyModuleNames.AddRange(new[] { "Json", "RenderCore", "RHI", "PhysicsCore", "Chaos", "ChaosCore", "GeometryCore", "Slate", "SlateCore", "InputCore", "ApplicationCore", "Niagara", "AudioMixer", "AudioExtensions", "Synthesis", "AssetRegistry", "XmlParser", "NiagaraShader", "StaticMeshDescription", "MeshDescription", "ImageWrapper" });
        // Read at run time from Config (FSCCargoDroneArt, FSCCargoFx, the live-spawn default fleet): staged with a packaged build.
        RuntimeDependencies.Add("$(ProjectDir)/Config/Cargo/drone_art.json", StagedFileType.UFS);
        RuntimeDependencies.Add("$(ProjectDir)/Config/Cargo/fleet.json", StagedFileType.UFS);
        // The RPC routes exist where the harness does (SCBridge.Build.cs: desktop, not Shipping).
        bool bRpc = Target.Configuration != UnrealTargetConfiguration.Shipping &&
                    (Target.Platform == UnrealTargetPlatform.Win64 || Target.Platform == UnrealTargetPlatform.Linux || Target.Platform == UnrealTargetPlatform.Mac);
        PrivateDefinitions.Add("SC_WITH_RPC=" + (bRpc ? "1" : "0"));
    }
}
