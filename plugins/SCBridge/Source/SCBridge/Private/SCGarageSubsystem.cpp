// WP-M2. The fleet describes entities. World placement and ownership remain the viewer and Ship Link's responsibility.
#include "SCGarageSubsystem.h"
#include "Cargo/SCCargoSubsystem.h"
#include "SCEnvironment.h"
#include "SCFirstPersonCharacter.h"
#include "SCShipLinkSubsystem.h"
#include "SCVehicle.h"
#include "SCViewerSettings.h"
#include "SCViewerSubsystem.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

FString FSCGaragePolicy::StoreRefusal(bool bOwned, bool bAboard, bool bTransfer)
{
    if (!bOwned) return TEXT("Owner access required");
    if (bAboard) return TEXT("Leave the ship before storing it");
    if (bTransfer) return TEXT("A cargo transfer is using this ship");
    return {};
}

FString FSCGaragePolicy::BayRefusal(bool bOwned, bool bConfirmed, bool& bNeedsConfirmation)
{
    bNeedsConfirmation = bOwned && !bConfirmed;
    if (!bOwned) return TEXT("The bay is occupied by another owner");
    return bNeedsConfirmation ? FString(TEXT("Store the ship occupying the bay first")) : FString();
}

bool FSCGaragePolicy::IsFleetCandidate(const FSCEntityHit& Entity)
{
    const FString Name = Entity.DisplayName.TrimStartAndEnd();
    // The game localization marks internal placeholders with <= ... =>. Their geometry makes them browser
    // candidates, but they are not named fleet entries.
    const bool bPlaceholder = Name.StartsWith(TEXT("<=")) && Name.EndsWith(TEXT("=>"));
    return Entity.Category == TEXT("Ships") && !Entity.GeometryPath.IsEmpty() && !bPlaceholder;
}

bool FSCGaragePolicy::IsVehicleKiosk(const FSCUiBinding& Binding)
{
    return Binding.IsOperatingPanel() && Binding.bOperated &&
        Binding.Buttons.ContainsByPredicate([](const FSCUiButton& B) { return B.Interaction == FName(TEXT("UseConsole")); }) &&
        Binding.StateTypes.ContainsByPredicate([](const FSCUiStateType& S) {
            return S.States.Contains(TEXT("VehicleList")) && S.States.Contains(TEXT("RetrievingVehicle"));
        });
}

const TCHAR* FSCGaragePolicy::StatusName(ESCGarageStatus Status)
{
    switch (Status)
    {
    case ESCGarageStatus::Retrieving: return TEXT("Retrieving");
    case ESCGarageStatus::InHangar: return TEXT("In hangar");
    case ESCGarageStatus::Unavailable: return TEXT("Unavailable");
    default: return TEXT("Stored");
    }
}

USCGarageSubsystem* USCGarageSubsystem::Get(const UObject* WorldContext)
{
    UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
    return World && World->IsGameWorld() ? World->GetSubsystem<USCGarageSubsystem>() : nullptr;
}

USCViewerSubsystem* USCGarageSubsystem::Viewer() const { return USCViewerSubsystem::Get(this); }

bool USCGarageSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
    return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId USCGarageSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(USCGarageSubsystem, STATGROUP_Tickables);
}

FString USCGarageSubsystem::Canonical(FString Entity)
{
    Entity.TrimStartAndEndInline();
    Entity.RemoveFromStart(TEXT("EntityClassDefinition."), ESearchCase::IgnoreCase);
    return Entity;
}

void USCGarageSubsystem::RequestCatalog()
{
    if (!bScannedCooked)
    {
        bScannedCooked = true;
        IAssetRegistry& Assets = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        TArray<FAssetData> LevelCandidates;
        Assets.GetAssetsByPath(FName(TEXT("/Game/SC/Ships/Maps")), LevelCandidates, false);
        for (const FAssetData& Level : LevelCandidates)
            if (Level.AssetClassPath == UWorld::StaticClass()->GetClassPathName())
            {
                const FString Entity = FPackageName::GetShortName(Level.PackageName);
                if (!USCViewerSubsystem::CookedShipLevel(Entity).IsEmpty()) Cooked.AddUnique(Entity);
            }
    }
    USCViewerSubsystem* V = Viewer();
    if (!V || V->GetArchiveState() != ESCArchiveState::Ready || V->IsShowcase() ||
        RequestedGeneration == V->GetArchiveGeneration()) return;
    RequestedGeneration = V->GetArchiveGeneration();
    Catalog.Reset();
    Details.Reset();
    DetailsQueue.Reset();
    ManufacturerCodes.Reset();
    bDetailsReading = false;
    V->SearchAsync(FString(), MAX_int32, [Weak = TWeakObjectPtr<USCGarageSubsystem>(this), Generation = RequestedGeneration](const FSCSearchResult& Result) {
        USCGarageSubsystem* Garage = Weak.Get();
        if (!Garage || !Garage->Viewer() || Garage->Viewer()->GetArchiveGeneration() != Generation || !Result.Error.IsEmpty()) return;
        for (const FSCEntityHit& Item : Result.Hits)
            if (FSCGaragePolicy::IsFleetCandidate(Item)) Garage->Catalog.Add(Item);
    });
}


void USCGarageSubsystem::RequestDetails(const FString& Name)
{
    const FString Entity = Canonical(Name);
    if (Entity.IsEmpty() || Details.Contains(FName(*Entity))) return;
    Details.Add(FName(*Entity));
    DetailsQueue.Add(Entity);
}

void USCGarageSubsystem::ReadNextDetail()
{
    USCViewerSubsystem* V = Viewer();
    if (bDetailsReading || DetailsQueue.IsEmpty() || !V || !V->GetClient() || V->GetArchiveState() != ESCArchiveState::Ready ||
        V->IsBusy() || V->IsSpawningVehicles() || V->IsEnvironmentLoading()) return;
    const FString Entity = MoveTemp(DetailsQueue[0]);
    DetailsQueue.RemoveAt(0, EAllowShrinking::No);
    const FName Key(*Entity);
    const int32 Generation = V->GetArchiveGeneration();
    const TWeakObjectPtr<USCGarageSubsystem> Weak(this);
    bDetailsReading = true;
    V->GetClient()->DataCoreRecord(TEXT("EntityClassDefinition.") + Entity, [Weak, Key, Generation](const FSCTextResult& Record) {
        USCGarageSubsystem* G = Weak.Get();
        if (!G || !G->Viewer() || G->Viewer()->GetArchiveGeneration() != Generation) return;
        const auto Finish = [&] { G->bDetailsReading = false; };
        TSharedPtr<FJsonObject> Class;
        if (!Record.bOK || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Record.Json), Class) || !Class)
        {
            Finish();
            return;
        }
        FDetails& Item = G->Details.FindOrAdd(Key);
        Class->TryGetStringField(TEXT("vehicleRole"), Item.Role);
        if (Item.Role.StartsWith(TEXT("@"))) Item.Role.Reset(); // the live record carries the key only
        FString ManufacturerRef;
        const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
        if (Class->TryGetArrayField(TEXT("Components"), Components) && Components)
            for (const auto& Value : *Components)
            {
                const TSharedPtr<FJsonObject>* Component = nullptr;
                const TSharedPtr<FJsonObject>* Attach = nullptr;
                if (!Value || !Value->TryGetObject(Component) || !Component || !*Component ||
                    !(*Component)->TryGetObjectField(TEXT("AttachDef"), Attach) || !Attach || !*Attach) continue;
                (*Attach)->TryGetStringField(TEXT("Manufacturer"), ManufacturerRef);
                double Size = 0;
                if ((*Attach)->TryGetNumberField(TEXT("Size"), Size) && Size > 0) Item.Size = FString::FromInt(int32(Size));
                break;
            }
        if (ManufacturerRef.IsEmpty() || !G->Viewer()->GetClient()) { Finish(); return; }
        if (const FString* Code = G->ManufacturerCodes.Find(ManufacturerRef)) { Item.Manufacturer = *Code; Finish(); return; }
        G->Viewer()->GetClient()->DataCoreRecord(ManufacturerRef, [Weak, Key, Generation, ManufacturerRef](const FSCTextResult& Manufacturer) {
            USCGarageSubsystem* Garage = Weak.Get();
            if (!Garage || !Garage->Viewer() || Garage->Viewer()->GetArchiveGeneration() != Generation) return;
            FString Code;
            TSharedPtr<FJsonObject> Result;
            if (Manufacturer.bOK && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Manufacturer.Json), Result) && Result)
                Result->TryGetStringField(TEXT("Code"), Code);
            Garage->ManufacturerCodes.Add(ManufacturerRef, Code);
            Garage->Details.FindOrAdd(Key).Manufacturer = MoveTemp(Code);
            Garage->bDetailsReading = false;
        });
    });
}

bool USCGarageSubsystem::Available(const FString& Entity) const
{
    return Cooked.ContainsByPredicate([&](const FString& Ship) { return Ship.Equals(Entity, ESearchCase::IgnoreCase); }) ||
        Catalog.ContainsByPredicate([&](const FSCEntityHit& Ship) { return Ship.Name.Equals(Entity, ESearchCase::IgnoreCase); });
}

TArray<FSCGarageRow> USCGarageSubsystem::Rows(const FString& EntityFilter, bool bIncludeDefaultStored) const
{
    const USCViewerSubsystem* V = Viewer();
    const USCShipLinkSubsystem* Link = USCShipLinkSubsystem::Get(this);
    const USCViewerSettings* Settings = USCViewerSettings::Get();
    TArray<FString> Names = Settings->GarageFleet;
    if (Names.IsEmpty() && bIncludeDefaultStored)
    {
        Names = Cooked;
        for (const FSCEntityHit& Hit : Catalog) Names.Add(Hit.Name);
    }
    const TArray<FSCVehicleInstanceInfo> Instances = V ? V->GetVehicleInstances() : TArray<FSCVehicleInstanceInfo>();
    TMap<FName, const FSCVehicleInstanceInfo*> Paired;
    if (Link)
        for (const auto& Info : Instances)
            if (Link->RoleOf(Link->GetPlayer(), Info.Actor.Get()) == ESCShipRole::Owner)
            {
                const FString Name = Canonical(Info.Entity);
                const FName Key(*Name);
                if (!Paired.Contains(Key)) Paired.Add(Key, &Info);
                Names.Add(Name);
            }
    if (Pending.IsSet()) Names.Add(Pending->Entity);
    for (const auto& Failure : Failures) Names.Add(Failure.Key);
    if (!EntityFilter.IsEmpty())
        Names.RemoveAll([&](const FString& Name) { return !Canonical(Name).Equals(EntityFilter, ESearchCase::IgnoreCase); });

    TMap<FName, const FSCEntityHit*> Hits;
    if (Names.Num() > 16)
    {
        Hits.Reserve(Catalog.Num());
        for (const auto& Hit : Catalog) Hits.Add(FName(*Hit.Name), &Hit);
    }
    TArray<FSCGarageRow> Out;
    TSet<FName> Seen;
    const ASCFirstPersonCharacter* Player = V ? V->GetFirstPerson() : nullptr;
    for (const FString& Name : Names)
    {
        const FString Entity = Canonical(Name);
        const FName Key(*Entity);
        if (Key.IsNone() || Seen.Contains(Key)) continue;
        Seen.Add(Key);
        FSCGarageRow& Row = Out.AddDefaulted_GetRef();
        Row.Entity = Entity;
        const FSCEntityHit* Hit = Hits.IsEmpty()
            ? Catalog.FindByPredicate([&](const FSCEntityHit& Item) { return Item.Name.Equals(Entity, ESearchCase::IgnoreCase); })
            : Hits.FindRef(Key);
        if (Hit)
            Row.DisplayName = Hit->DisplayName;
        if (Row.DisplayName.IsEmpty()) Row.DisplayName = Entity.Replace(TEXT("_"), TEXT(" "));
        if (const FDetails* Metadata = Details.Find(Key))
        {
            Row.Manufacturer = Metadata->Manufacturer;
            Row.Role = Metadata->Role;
            Row.Size = Metadata->Size;
        }
        Row.Error = Failures.FindRef(Entity.ToLower());
        if (!Row.Error.IsEmpty()) Row.Status = ESCGarageStatus::Unavailable;
        if (Pending.IsSet() && Pending->Entity.Equals(Entity, ESearchCase::IgnoreCase))
        {
            Row.Status = ESCGarageStatus::Retrieving;
            if (Pending->Id == 0 && V) Row.Progress = V->GetLoadProgress();
            else if (const auto* Info = Instances.FindByPredicate([&](const auto& I) { return I.Id == Pending->Id; }))
                Row.Progress = Info->Progress;
        }
        if (const auto* Info = Paired.FindRef(Key))
        {
            const ASCVehicle* Ship = Info->Actor.Get();
            if (!Ship) continue;
            Row.Status = ESCGarageStatus::InHangar;
            Row.Ship = Info->Id;
            Row.Progress = 1;
            Row.Error.Reset();
            Row.DistanceM = Player ? FVector::Dist(Ship->GetActorLocation(), Player->GetActorLocation()) / 100.0 : 0;
            const FSCSceneInstance& Root = Ship->Manifest.Root;
            if (!Root.DisplayName.IsEmpty()) Row.DisplayName = Root.DisplayName;
            if (Row.Role.IsEmpty()) Row.Role = Root.ItemSubType;
            if (Row.Size.IsEmpty()) Row.Size = Root.ItemSize;
            const int32 ActivePaint = Ship->GetPaint();
            Row.Paint = Ship->Manifest.Paints.IsValidIndex(ActivePaint) ? Ship->Manifest.Paints[ActivePaint].PaletteId : TEXT("default");
            for (const FSCPaintVariant& Paint : Ship->Manifest.Paints) Row.PaintOptions.Add(Paint.PaletteId);
        }
    }
    Out.Sort([](const FSCGarageRow& A, const FSCGarageRow& B) {
        const bool bAActive = A.Status == ESCGarageStatus::InHangar || A.Status == ESCGarageStatus::Retrieving;
        const bool bBActive = B.Status == ESCGarageStatus::InHangar || B.Status == ESCGarageStatus::Retrieving;
        if (bAActive != bBActive) return bAActive;
        return A.DisplayName == B.DisplayName ? A.Entity < B.Entity : A.DisplayName < B.DisplayName;
    });
    return Out;
}

bool USCGarageSubsystem::AddToFleet(const FString& Name)
{
    const FString Entity = Canonical(Name);
    if (!Available(Entity)) return false;
    USCViewerSettings* Settings = USCViewerSettings::Get();
    if (!Settings->GarageFleet.ContainsByPredicate([&](const FString& InFleet) { return InFleet.Equals(Entity, ESearchCase::IgnoreCase); }))
    {
        Settings->GarageFleet.Add(Entity);
        Settings->Save();
    }
    return true;
}

FSCGarageOrder USCGarageSubsystem::Retrieve(const FString& Name, ESCGaragePlacement At, bool bConfirmStore, int32 ConfirmedOccupant)
{
    USCViewerSubsystem* V = Viewer();
    USCShipLinkSubsystem* Link = USCShipLinkSubsystem::Get(this);
    FSCGarageOrder Result;
    auto Refuse = [&](const FString& Why) { Result.Refusal = Why; return Result; };
    if (!V || !Link) return Refuse(TEXT("The garage is unavailable"));
    if (Pending.IsSet() || V->IsBusy() || V->IsSpawningVehicles() || V->IsEnvironmentLoading()) return Refuse(TEXT("Placement is busy"));
    if (V->IsSpace()) return Refuse(TEXT("Retrieve ships from a hangar"));
    RequestCatalog();
    const FString Entity = Canonical(Name);
    const TArray<FSCGarageRow> Fleet = Rows(Entity);
    const auto* Row = Fleet.FindByPredicate([&](const FSCGarageRow& R) { return R.Entity.Equals(Entity, ESearchCase::IgnoreCase); });
    if (!Row) return Refuse(TEXT("This ship is not in your fleet"));
    if (Row->Status == ESCGarageStatus::InHangar) return Refuse(TEXT("This ship is already in the hangar"));
    if (!V->CanLoad(Entity)) return Refuse(TEXT("The ship is unavailable in this build"));

    FBox Bay(ForceInit);
    const bool bHasBay = At == ESCGaragePlacement::Bay && FSCEnvironment::FindShipBay(V->GetEnvironment(), Bay);
    if (At == ESCGaragePlacement::Bay)
        for (ASCVehicle* Occupant : V->GetVehicles())
        {
            const FBox Current = FSCEnvironment::SolidBounds(Occupant);
            const bool bOccupies = bHasBay
                ? Current.IsValid && Current.IntersectXY(Bay) && FMath::IsNearlyEqual(Current.Min.Z, Bay.Max.Z, 100.0)
                : Occupant == V->GetVehicle();
            if (!bOccupies) continue;
            Result.Occupant = V->GetVehicleInstanceId(Occupant);
            const bool bOwned = Link->RoleOf(Link->GetPlayer(), Occupant) == ESCShipRole::Owner;
            const bool bConfirmedThisShip = bConfirmStore && (ConfirmedOccupant == INDEX_NONE || ConfirmedOccupant == Result.Occupant);
            const FString BlockingReason = FSCGaragePolicy::BayRefusal(bOwned, bConfirmedThisShip, Result.bConfirmStore);
            if (!BlockingReason.IsEmpty()) return Refuse(BlockingReason);
            const FString Stored = Store(Result.Occupant);
            if (!Stored.IsEmpty()) return Refuse(Stored);
            break;
        }

    if (At == ESCGaragePlacement::Bay && !V->GetVehicle())
    {
        if (!V->LoadGarageVehicle(Entity)) return Refuse(TEXT("Could not start retrieval"));
        Result.Ship = 0;
    }
    else
    {
        if (V->GetArchiveState() != ESCArchiveState::Ready) return Refuse(TEXT("Extra instances require an open archive"));
        if (bHasBay)
        {
            FVector Top = Bay.GetCenter();
            Top.Z = Bay.Max.Z;
            Result.Ship = V->SpawnVehicleInstance(Entity, FTransform(Top));
        }
        else Result.Ship = V->SpawnGarageFloorVehicle(Entity);
        if (Result.Ship == INDEX_NONE) return Refuse(TEXT("Could not queue an extra vehicle"));
    }
    Pending = FPending{Entity, Result.Ship};
    Failures.Remove(Entity.ToLower());
    return Result;
}

FString USCGarageSubsystem::Store(int32 Id)
{
    USCViewerSubsystem* V = Viewer();
    USCShipLinkSubsystem* Link = USCShipLinkSubsystem::Get(this);
    ASCVehicle* Ship = Link ? Link->FindShip(Id) : nullptr;
    if (!Ship || !V) return TEXT("No such ship");
    const bool bOwned = Link->RoleOf(Link->GetPlayer(), Ship) == ESCShipRole::Owner;
    const ASCFirstPersonCharacter* Player = V->GetFirstPerson();
    bool bAboard = false;
    if (Player)
    {
        const FVector At = Player->GetActorLocation();
        const UCapsuleComponent* Capsule = Player->GetCapsuleComponent();
        const FVector Feet = Capsule ? At - FVector(0, 0, Capsule->GetScaledCapsuleHalfHeight()) : At;
        const UPrimitiveComponent* Base = Cast<UPrimitiveComponent>(Player->GetMovementBaseObject());
        bAboard = (Base && Base->GetOwner() == Ship) || Ship->IsInPhysicsGrid(At) || Ship->IsInsideLightArea(At) ||
            Ship->LiftRiddenBy(Feet, Base) != INDEX_NONE;
    }
    const USCCargoSubsystem* Cargo = USCCargoSubsystem::Get(this);
    const FString Refusal = FSCGaragePolicy::StoreRefusal(bOwned, bAboard, Cargo && Cargo->IsVehicleTransferActive(Ship));
    if (!Refusal.IsEmpty()) return Refusal;
    return (Id == 0 ? V->StoreGaragePrimary() : V->DestroyVehicleInstance(Id)) ? FString() : FString(TEXT("The ship could not be stored"));
}

FString USCGarageSubsystem::PaintEntity(const FString& Entity, const FString& Palette)
{
    const TArray<FSCGarageRow> Fleet = Rows(Entity);
    const auto* Row = Fleet.FindByPredicate([&](const FSCGarageRow& R) { return R.Entity.Equals(Entity, ESearchCase::IgnoreCase); });
    if (!Row) return TEXT("This ship is not in your fleet");
    return Row->Ship != INDEX_NONE ? Paint(Row->Ship, Palette) : FString(TEXT("Retrieve the ship before choosing its paint"));
}

FString USCGarageSubsystem::Paint(int32 Id, const FString& Palette)
{
    USCViewerSubsystem* V = Viewer();
    USCShipLinkSubsystem* Link = USCShipLinkSubsystem::Get(this);
    ASCVehicle* Ship = Link ? Link->FindShip(Id) : nullptr;
    if (!V || !Ship || Link->RoleOf(Link->GetPlayer(), Ship) != ESCShipRole::Owner) return TEXT("Owner access required");
    const bool bDefault = Palette.IsEmpty() || Palette.Equals(TEXT("default"), ESearchCase::IgnoreCase);
    const int32 Index = bDefault ? INDEX_NONE : Ship->Manifest.Paints.IndexOfByPredicate([&](const FSCPaintVariant& P) {
        return P.PaletteId.Equals(Palette, ESearchCase::IgnoreCase) || P.Label().Equals(Palette, ESearchCase::IgnoreCase);
    });
    if (!bDefault && Index == INDEX_NONE) return TEXT("This ship does not have that paint");
    if (!Ship->SetPaint(Index) && Ship->GetPaint() != Index) return TEXT("This instance cannot apply the paint");
    const TArray<FSCVehicleInstanceInfo> Instances = V->GetVehicleInstances();
    const auto* Info = Instances.FindByPredicate([Id](const auto& I) { return I.Id == Id; });
    if (!Info) return TEXT("The ship is gone");
    const FString Entity = Canonical(Info->Entity);
    USCViewerSettings* Settings = USCViewerSettings::Get();
    if (bDefault) Settings->Paints.Remove(Entity);
    else Settings->Paints.Add(Entity, Ship->Manifest.Paints[Index].PaletteId);
    Settings->Save();
    return {};
}

void USCGarageSubsystem::FinishRetrieve(const FString& Error, ASCVehicle* Ship)
{
    const FString Entity = Pending->Entity;
    Pending.Reset();
    if (!Error.IsEmpty()) Failures.Add(Entity.ToLower(), Error);
    if (Ship)
        if (const FString* Saved = USCViewerSettings::Get()->Paints.Find(Entity))
        {
            const int32 Index = Ship->Manifest.Paints.IndexOfByPredicate([&](const FSCPaintVariant& P) { return P.PaletteId == *Saved; });
            if (Index != INDEX_NONE) Ship->SetPaint(Index);
        }
}

void USCGarageSubsystem::Tick(float)
{
    if (!Pending.IsSet()) { ReadNextDetail(); return; }
    USCViewerSubsystem* V = Viewer();
    if (!V) return;
    if (Pending->Id == 0)
    {
        if (V->IsBusy()) return;
        const FSCLoadReport Report = V->GetLastReport();
        if (Report.Result == ESCLoadPhase::Succeeded && V->GetLoadedEntity().Equals(Pending->Entity, ESearchCase::IgnoreCase))
            FinishRetrieve(FString(), V->GetVehicle());
        else FinishRetrieve(Report.Error.IsEmpty() ? FString(TEXT("Retrieval did not finish")) : Report.Error, nullptr);
        return;
    }
    const TArray<FSCVehicleInstanceInfo> Instances = V->GetVehicleInstances();
    const auto* Info = Instances.FindByPredicate([&](const auto& I) { return I.Id == Pending->Id; });
    if (!Info) FinishRetrieve(TEXT("The queued ship was removed"), nullptr);
    else if (Info->State == ESCVehicleInstanceState::Failed) FinishRetrieve(Info->Error, nullptr);
    else if (Info->State == ESCVehicleInstanceState::Ready) FinishRetrieve(FString(), Info->Actor.Get());
}

bool ASCVehicle::AimGarageKiosk(const FVector& Eye, const FVector& Direction, float Reach, const AActor* Ignore) const
{
    if (GarageKiosks.IsEmpty() || !GetWorld()) return false;
    float Longest = Reach;
    for (const auto& Entry : GarageKiosks) Longest = FMath::Max(Longest, Entry.Value);
    FHitResult Hit;
    const FCollisionQueryParams Params(SCENE_QUERY_STAT(SCGarageKiosk), true, Ignore);
    if (!GetWorld()->LineTraceSingleByChannel(Hit, Eye, Eye + Direction.GetSafeNormal() * Longest, ECC_Visibility, Params) || Hit.GetActor() != this)
        return false;
    for (USceneComponent* Component = Hit.GetComponent(); Component; Component = Component->GetAttachParent())
        if (const float* KioskReach = GarageKiosks.Find(Component))
            return Hit.Distance <= FMath::Max(Reach, *KioskReach);
    return false;
}
