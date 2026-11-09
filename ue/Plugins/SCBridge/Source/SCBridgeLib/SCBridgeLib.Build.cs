using UnrealBuildTool;
public class SCBridgeLib : ModuleRules
{
    public SCBridgeLib(ReadOnlyTargetRules Target) : base(Target)
    {
        Type = ModuleType.External;
        // WP-R4 supplies the C ABI library and headers here. WP-U0 runs no cargo.
    }
}
