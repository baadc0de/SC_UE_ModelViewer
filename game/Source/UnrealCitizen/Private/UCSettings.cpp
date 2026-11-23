// UUCSettings: cargo file resolution and persistence.
#include "UCSettings.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "HAL/PlatformMisc.h"

UUCSettings* UUCSettings::Get()
{
    return GetMutableDefault<UUCSettings>();
}
void UUCSettings::Save()
{
    SaveConfig();
}
namespace
{
/** A cargo JSON a setting or switch names: a bare name or a path under Config/Cargo, else relative to the launch
 *  directory; empty or "none" = none. */
FString ResolveCargoFile(FString Value)
{
    Value.TrimStartAndEndInline();
    if (Value.IsEmpty() || Value.Equals(TEXT("none"), ESearchCase::IgnoreCase)) return FString();
    if (FPaths::IsRelative(Value))
    {
        // A bare name or a path under Config/Cargo; else relative to the launch directory.
        const FString InConfig = FPaths::Combine(FPaths::ProjectConfigDir(), TEXT("Cargo"), Value);
        const FString Named = FPaths::GetExtension(InConfig).IsEmpty() ? InConfig + TEXT(".json") : InConfig;
        Value = FPaths::FileExists(Named) ? Named : FPaths::ConvertRelativePathToFull(FPlatformMisc::LaunchDir(), Value);
    }
    return FPaths::ConvertRelativePathToFull(Value);
}
} // namespace
FString UUCSettings::ResolveCargoScenario() const
{
    FString Value = CargoScenario;
    FParse::Value(FCommandLine::Get(), TEXT("SCCargoScenario="), Value);
    return ResolveCargoFile(Value);
}
FString UUCSettings::ResolveScenario() const
{
    FString Value = Scenario;
    FParse::Value(FCommandLine::Get(), TEXT("Scenario="), Value);
    Value.TrimStartAndEndInline();
    return Value.Equals(TEXT("none"), ESearchCase::IgnoreCase) ? FString() : Value;
}
FString UUCSettings::ResolveCargoFleet() const
{
    FString Value = CargoFleet;
    FParse::Value(FCommandLine::Get(), TEXT("SCCargoFleet="), Value);
    return ResolveCargoFile(Value);
}
