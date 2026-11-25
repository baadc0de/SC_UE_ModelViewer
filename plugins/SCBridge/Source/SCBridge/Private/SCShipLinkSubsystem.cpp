// WP-M1: one owner store per world and one set of commands shared by Ship Link and RPC.
#include "SCShipLinkSubsystem.h"
#include "SCVehicle.h"
#include "SCViewerSubsystem.h"
#include "Engine/World.h"

ESCShipDoorGroup FSCShipDoorPolicy::Classify(bool bHasRooms, const FString& SideA, const FString& SideB)
{
    if (!bHasRooms || (SideA.IsEmpty() && SideB.IsEmpty())) return ESCShipDoorGroup::Other;
    return SideA.IsEmpty() != SideB.IsEmpty() ? ESCShipDoorGroup::Exterior : ESCShipDoorGroup::Interior;
}

FString FSCShipDoorPolicy::Refusal(ESCShipRole Role, ESCShipDoorAction Action, bool bLocked, ESCShipDoorGroup Group, bool bInside)
{
    if (Action == ESCShipDoorAction::Lock || Action == ESCShipDoorAction::Unlock)
        return Role == ESCShipRole::Owner ? FString() : FString(TEXT("Owner access required"));
    if (Action == ESCShipDoorAction::Open && bLocked && !(Role == ESCShipRole::Owner && Group == ESCShipDoorGroup::Exterior && bInside))
        return TEXT("Locked");
    return {};
}

const TCHAR* FSCShipDoorPolicy::GroupName(ESCShipDoorGroup Group)
{
    switch (Group)
    {
    case ESCShipDoorGroup::Exterior: return TEXT("Exterior");
    case ESCShipDoorGroup::Interior: return TEXT("Interior");
    default: return TEXT("Other");
    }
}

USCShipLinkSubsystem* USCShipLinkSubsystem::Get(const UObject* WorldContext)
{
    UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
    return World && World->IsGameWorld() ? World->GetSubsystem<USCShipLinkSubsystem>() : nullptr;
}

void USCShipLinkSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
    Super::OnWorldBeginPlay(InWorld);
    if (Viewer.IsValid()) return;
    Viewer = USCViewerSubsystem::Get(&InWorld);
    if (USCViewerSubsystem* V = Viewer.Get()) V->OnVehiclesChanged.AddUObject(this, &USCShipLinkSubsystem::SyncVehicles);
    SyncVehicles();
}

void USCShipLinkSubsystem::Deinitialize()
{
    if (USCViewerSubsystem* V = Viewer.Get()) V->OnVehiclesChanged.RemoveAll(this);
    Pairings.Reset();
    LocateShip.Reset();
    Super::Deinitialize();
}

void USCShipLinkSubsystem::SyncVehicles()
{
    USCViewerSubsystem* V = Viewer.Get();
    if (!V) return;
    TSet<int32> Present;
    for (const FSCVehicleInstanceInfo& Info : V->GetVehicleInstances())
    {
        if (Info.State == ESCVehicleInstanceState::Failed) continue;
        Present.Add(Info.Id);
        FPairing& Pair = Pairings.FindOrAdd(Info.Id);
        ASCVehicle* Previous = Pair.Vehicle.Get();
        ASCVehicle* Current = Info.Actor.Get();
        if ((Previous || Pair.Vehicle.IsStale()) && Previous != Current)
        {
            Pair.ExplicitOwner = NAME_None;
            Pair.bUnowned = false;
        }
        Pair.Vehicle = Current;
    }
    for (auto It = Pairings.CreateIterator(); It; ++It)
        if (!Present.Contains(It.Key())) It.RemoveCurrent();
    if (GetOwner(LocateShip.Get()) != Player) LocateShip.Reset();
}

void USCShipLinkSubsystem::SetScenarioPlayer(FName Actor)
{
    if (!Actor.IsNone()) Player = Actor;
}

void USCShipLinkSubsystem::SetOwner(int32 VehicleInstance, FName Actor)
{
    if (VehicleInstance >= 0)
    {
        FPairing& Pair = Pairings.FindOrAdd(VehicleInstance);
        Pair.ExplicitOwner = Actor;
        Pair.bUnowned = false;
    }
    if (GetOwner(LocateShip.Get()) != Player) LocateShip.Reset();
}

void USCShipLinkSubsystem::SetUnowned(int32 VehicleInstance)
{
    if (VehicleInstance >= 0)
    {
        FPairing& Pair = Pairings.FindOrAdd(VehicleInstance);
        Pair.ExplicitOwner = NAME_None;
        Pair.bUnowned = true;
    }
    if (GetOwner(LocateShip.Get()) != Player) LocateShip.Reset();
}

FName USCShipLinkSubsystem::GetOwner(const ASCVehicle* Vehicle) const
{
    if (!Vehicle || !Viewer.IsValid()) return NAME_None;
    const FPairing* Pair = Pairings.Find(Viewer->GetVehicleInstanceId(Vehicle));
    if (!Pair || Pair->Vehicle.Get() != Vehicle || Pair->bUnowned) return NAME_None;
    return Pair->ExplicitOwner.IsNone() ? Player : Pair->ExplicitOwner;
}

ESCShipRole USCShipLinkSubsystem::RoleOf(FName Actor, const ASCVehicle* Vehicle) const
{
    return !Actor.IsNone() && GetOwner(Vehicle) == Actor ? ESCShipRole::Owner : ESCShipRole::None;
}

TArray<ASCVehicle*> USCShipLinkSubsystem::PairedShips(FName Actor) const
{
    TArray<ASCVehicle*> Out;
    if (const USCViewerSubsystem* V = Viewer.Get())
        for (ASCVehicle* Ship : V->GetVehicles())
            if (RoleOf(Actor, Ship) == ESCShipRole::Owner) Out.Add(Ship);
    return Out;
}

ASCVehicle* USCShipLinkSubsystem::FindShip(int32 VehicleInstance) const
{
    const FPairing* Pair = Pairings.Find(VehicleInstance);
    return Pair ? Pair->Vehicle.Get() : nullptr;
}

FString USCShipLinkSubsystem::OrderDoor(ASCVehicle* Ship, int32 Door, ESCShipDoorAction Action, FName Actor,
    const TOptional<FVector>& LocalUse)
{
    if (!Ship) return TEXT("No such ship");
    return Ship->OrderShipDoor(Door, Action, Actor, LocalUse);
}

TArray<FSCShipDoorResult> USCShipLinkSubsystem::Secure(ASCVehicle* Ship)
{
    return Ship ? Ship->SecureShip(Player) : TArray<FSCShipDoorResult>();
}

FString USCShipLinkSubsystem::SetLight(ASCVehicle* Ship, int32 Group, const FString& Mode)
{
    if (!Ship || !Ship->LightGroups.IsValidIndex(Group)) return TEXT("No such light group");
    const FSCLightGroupInteraction* Interaction = Ship->LightGroups[Group].Interactions.FindByPredicate(
        [&](const FSCLightGroupInteraction& Candidate) { return Candidate.LightState.Equals(Mode, ESearchCase::IgnoreCase); });
    if (!Interaction || Interaction->LinkingTag.IsEmpty()) return TEXT("Mode unavailable");
    FString State;
    Ship->DeliverLightGroupTag(Group, Interaction->LinkingTag, &State);
    return State.IsEmpty() ? FString(TEXT("Mode unavailable")) : FString();
}

TArray<int32> USCShipLinkSubsystem::SetAllLights(ASCVehicle* Ship, const FString& Mode)
{
    TArray<int32> Skipped;
    if (!Ship) return Skipped;
    auto IsMode = [](const FSCLightGroupInteraction& Interaction, bool bOff) {
        return !Interaction.LightState.IsEmpty() && !Interaction.LinkingTag.IsEmpty() &&
            Interaction.LightState.Equals(TEXT("Off"), ESearchCase::IgnoreCase) == bOff;
    };
    const bool bToggle = Mode.IsEmpty();
    const bool bTurnOff = Mode.Equals(TEXT("Off"), ESearchCase::IgnoreCase) ||
        (bToggle && Ship->LightGroups.ContainsByPredicate([&](const FSCLightGroup& Group) {
            return !Group.LightState.Equals(TEXT("Off"), ESearchCase::IgnoreCase) &&
                Group.Interactions.ContainsByPredicate([&](const FSCLightGroupInteraction& I) { return IsMode(I, true); }) &&
                Group.Interactions.ContainsByPredicate([&](const FSCLightGroupInteraction& I) { return IsMode(I, false); });
        }));
    for (int32 Id = 0; Id < Ship->LightGroups.Num(); ++Id)
    {
        const auto& Interactions = Ship->LightGroups[Id].Interactions;
        const auto* Off = Interactions.FindByPredicate([&](const FSCLightGroupInteraction& I) { return IsMode(I, true); });
        const auto* On = Interactions.FindByPredicate([&](const FSCLightGroupInteraction& I) { return IsMode(I, false); });
        const FString Target = bToggle && Off && On ? (bTurnOff ? Off->LightState : On->LightState) : Mode;
        if (Target.IsEmpty() || !SetLight(Ship, Id, Target).IsEmpty()) Skipped.Add(Id);
    }
    return Skipped;
}

bool USCShipLinkSubsystem::IsLocating(const ASCVehicle* Ship) const
{
    return Ship && LocateShip.Get() == Ship;
}

ASCVehicle* USCShipLinkSubsystem::GetLocatedShip() const { return LocateShip.Get(); }

void USCShipLinkSubsystem::ToggleLocate(ASCVehicle* Ship)
{
    LocateShip = !IsLocating(Ship) && RoleOf(Player, Ship) == ESCShipRole::Owner ? Ship : nullptr;
}
